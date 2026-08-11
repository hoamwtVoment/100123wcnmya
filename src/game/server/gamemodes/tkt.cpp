/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <engine/server.h>
#include <engine/shared/config.h>

#include <base/tl/string.h>

#include <game/collision.h>
#include <game/generated/server_data.h>
#include <game/server/entities/character.h>
#include <game/server/entities/pickup.h>
#include <game/server/entities/projectile.h>
#include <game/server/player.h>
#include <game/server/teams.h>
#include <game/server/weapons.h>

#include "../../../tools/tkt_mapgen.h"

#include "tkt.h"

CGameControllerTKT::CGameControllerTKT() :
	IGameController(),
	m_RespawnDelayTKT(3),
	m_RoundTimeTKT(60),
	m_InvincibleTicksTKT(50),
	m_PlayerHealthTKT(3),
	m_PlayerArmorTKT(1),
	m_GunSpeedTKT(825),
	m_ShotgunSpeedTKT(1238),
	m_GrenadeSpeedTKT(825),
	m_GunBounceTKT(8),
	m_ShotgunBounceTKT(4),
	m_GrenadeBounceTKT(6),
	m_NextItemSpawnTick(0),
	m_ReloadPending(false),
	m_SubMapCountTKT(10),
	m_CycleRound(0),
	m_IsMegaMap(false)
{
	m_pGameType = "tkt";
	m_GameFlags = IGF_SURVIVAL | IGF_ROUND_TIMER_ROUND | IGF_SUDDENDEATH;

	INSTANCE_CONFIG_INT(&m_RespawnDelayTKT, "respawn_delay_tkt", 3, 0, 10, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Time needed to respawn after death in tkt gametype")
	INSTANCE_CONFIG_INT(&m_RoundTimeTKT, "round_time_tkt", 60, 0, 600, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Round time for tkt gametype")
	INSTANCE_CONFIG_INT(&m_InvincibleTicksTKT, "invincible_ticks_tkt", 50, 0, 600, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Invincibility ticks after spawn in tkt gametype")
	INSTANCE_CONFIG_INT(&m_PlayerHealthTKT, "player_health_tkt", 3, 0, 10, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Player health in tkt gametype")
	INSTANCE_CONFIG_INT(&m_PlayerArmorTKT, "player_armor_tkt", 1, 0, 10, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Player armor cap in tkt gametype")
	INSTANCE_CONFIG_INT(&m_GunSpeedTKT, "gun_speed_tkt", 825, 1, 10000, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Gun bullet speed in pixels per second in tkt gametype")
	INSTANCE_CONFIG_INT(&m_ShotgunSpeedTKT, "shotgun_speed_tkt", 1238, 1, 10000, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Shotgun bullet speed in pixels per second in tkt gametype")
	INSTANCE_CONFIG_INT(&m_GrenadeSpeedTKT, "grenade_speed_tkt", 825, 1, 10000, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Grenade speed in pixels per second in tkt gametype")
	INSTANCE_CONFIG_INT(&m_GunBounceTKT, "gun_bounce_tkt", 8, 0, 50, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Max bounces for gun bullets in tkt gametype")
	INSTANCE_CONFIG_INT(&m_ShotgunBounceTKT, "shotgun_bounce_tkt", 4, 0, 50, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Max bounces for shotgun bullets in tkt gametype")
	INSTANCE_CONFIG_INT(&m_GrenadeBounceTKT, "grenade_bounce_tkt", 6, 0, 50, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Max bounces for grenades in tkt gametype")
	INSTANCE_CONFIG_INT(&m_SubMapCountTKT, "submap_count_tkt", 10, 1, 64, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "Maps played per cycle in tkt gametype, pre-generated into one mega map")

	// a match needs at least 2 players to start
	m_MinimumPlayers = 1; // TEMP TEST
}

// pre-generate a fresh mega map: m_SubMapCountTKT arenas sized by the
// current player count are packed into one tkt.map, the round-end map
// change loads it and each round then plays one submap without reconnecting
void CGameControllerTKT::GenerateMegaTKTMapForPlayers()
{
	int NumPlayers = 0;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		CPlayer *pPlayer = GetPlayerIfInRoom(i);
		if(pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS)
			NumPlayers++;
	}
	int TargetArea = 900 + (NumPlayers - 1) * ((100000 - 900) / 63);
	if(NumPlayers < 1)
		TargetArea = 900;

	int Width, Height;
	TKTMapSizeFromArea(TargetArea, Width, Height);
	WriteMegaTKTMapFile(GameServer()->Storage(), (uint64_t)secure_rand(), m_SubMapCountTKT, Width, Height, "maps/tkt.map");
}

void CGameControllerTKT::OnInit()
{
	// detect the mega map: the submap names are registered from the map
	// config by the engine on every map load; m_CycleRound must NOT be
	// reset here because soft reloads reuse this controller mid-cycle
	m_IsMegaMap = CGameTeams::GetMapIndex("tkt_1") > 0;

	// collect the item spawn points from the game layer (raw tile index 189);
	// on a mega map only the items of the active submap are usable
	m_ItemSpawns.clear();
	CMapItemLayerTilemap *pGameLayer = GameServer()->Layers()->GameLayer();
	if(pGameLayer && pGameLayer->m_Data >= 0)
	{
		CTile *pTiles = (CTile *)GameServer()->Layers()->Map()->GetData(pGameLayer->m_Data);
		int W = pGameLayer->m_Width;
		int H = pGameLayer->m_Height;

		CMapItemLayerTilemap *pSpeedupLayer = GameServer()->Layers()->SpeedupLayer();
		CSpeedupTile *pSpeedup = 0;
		int SW = 0, SH = 0;
		if(pSpeedupLayer && pSpeedupLayer->m_Speedup >= 0)
		{
			pSpeedup = (CSpeedupTile *)GameServer()->Layers()->Map()->GetData(pSpeedupLayer->m_Speedup);
			SW = pSpeedupLayer->m_Width;
			SH = pSpeedupLayer->m_Height;
		}

		for(int y = 0; y < H; ++y)
			for(int x = 0; x < W; ++x)
			{
				CTile Tile = pTiles[y * W + x];
				if(Tile.m_Index == 189)
				{
					if(m_MapIndex > 0 && pSpeedup && x < SW && y < SH)
					{
						CSpeedupTile &S = pSpeedup[y * SW + x];
						if(S.m_Type == TILE_MEGAMAP_INDEX && S.m_MaxSpeed != m_MapIndex)
							continue; // belongs to another submap
					}
					m_ItemSpawns.push_back(vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f));
				}
			}
	}

	// make sure the mega map exists: it is normally pre-generated on the
	// boot map, but a fresh server that boots directly into the mega map
	// needs it generated here (one file existence check per init)
	{
		IOHANDLE MapFile = GameServer()->Storage()->OpenFile("maps/tkt.map", IOFLAG_READ, IStorage::TYPE_ALL);
		if(!MapFile)
		{
			GenerateMegaTKTMapForPlayers();
			dbg_msg("tkt", "generated the mega map on init (file was missing)");
		}
		else
			io_close(MapFile);
	}
}

void CGameControllerTKT::OnControllerStart()
{
	// mid-cycle submap switches start the next round immediately: the soft
	// reload restarts the controller which would otherwise enter the warmup.
	// StartRound (not StartMatch) keeps m_RoundCount > 0, so the scores
	// from the previous rounds are not reset; only a fresh cycle (after the
	// map reload) goes through the warmup and starts a new match with
	// reset scores
	if(m_IsMegaMap && m_CycleRound > 0)
		StartRound();

	m_Timelimit = m_RoundTimeTKT;
	m_GameInfo.m_TimeLimit = m_RoundTimeTKT;
	m_NextItemSpawnTick = Server()->Tick() + 15 * Server()->TickSpeed();

	// straight bullets: zero the projectile curvature for all weapons
	CTuningParams *pTuning = GameServer()->Tuning();
	pTuning->m_GunCurvature = 0;
	pTuning->m_ShotgunCurvature = 0;
	pTuning->m_GrenadeCurvature = 0;
	GameServer()->SendTuningParams(-1, 0);
}

void CGameControllerTKT::OnGameStart(bool IsRound)
{
	// clear all spawned items and restart the spawn timer
	for(CEntity *pItem : m_ItemEntities)
		if(pItem)
			pItem->Destroy();
	m_ItemEntities.clear();
	m_NextItemSpawnTick = Server()->Tick() + 15 * Server()->TickSpeed();

	// the mega map is pre-generated at cycle start, nothing to do here
}

void CGameControllerTKT::OnWorldReset()
{
}

void CGameControllerTKT::OnPlayerJoin(class CPlayer *pPlayer)
{
}

void CGameControllerTKT::OnPlayerLeave(class CPlayer *pPlayer)
{
}

bool CGameControllerTKT::CanChangeTeam(class CPlayer *pPlayer, int JoinTeam) const
{
	// no team switching in tkt: only spectators may join the game
	return JoinTeam == TEAM_SPECTATORS || pPlayer->GetTeam() == TEAM_SPECTATORS;
}

bool CGameControllerTKT::OnPlayerTryRespawn(class CPlayer *pPlayer, vec2 Pos)
{
	return true;
}

void CGameControllerTKT::OnCharacterSpawn(class CCharacter *pChr)
{
	pChr->IncreaseHealth(m_PlayerHealthTKT);
	pChr->GiveWeapon(WEAPON_GUN, WEAPON_ID_PISTOL, 5);
	pChr->SetWeaponSlot(WEAPON_GUN, false);
	CWeapon *pPistol = pChr->CurrentWeapon();
	if(pPistol)
		pPistol->SetMaxAmmo(5);
	pChr->Protect(m_InvincibleTicksTKT / (float)Server()->TickSpeed(), false);

	// all weapons can hit characters, grenades explode on impact
	pChr->m_Hit = CCharacter::HIT_ALL;
}

int CGameControllerTKT::OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	return DEATH_NORMAL;
}

int CGameControllerTKT::OnCharacterTakeDamage(class CCharacter *pChr, vec2 &Force, int &Dmg, int From, int WeaponType, int WeaponID, bool IsExplosion)
{
	if(Dmg > 0)
	{
		Dmg = 1;
		// enter invincibility frames immediately after taking damage,
		// blocking further hits even within the same tick
		pChr->Protect(m_InvincibleTicksTKT / (float)Server()->TickSpeed(), false);
	}
	return DAMAGE_NORMAL;
}

bool CGameControllerTKT::OnCharacterTile(class CCharacter *pChr, int MapIndex)
{
	return false;
}

bool CGameControllerTKT::OnEntity(int Index, vec2 Pos, int Layer, int Flags, int Number)
{
	// bypass pickups, everyone starts with a pistol only
	// (item spawn points are collected directly from the game layer in OnInit)
	if(Index >= ENTITY_ARMOR_1 && Index <= ENTITY_WEAPON_LASER)
		return true;
	return false;
}

// item spawn list: shotgun, grenade, heart, shield
enum
{
	ITEM_SHOTGUN = 0,
	ITEM_GRENADE,
	ITEM_HEALTH,
	ITEM_ARMOR,
	NUM_ITEMS,
};

static const struct
{
	int Type;
	int SubType;
} s_aItemSpawnList[NUM_ITEMS] = {
	{POWERUP_WEAPON, WEAPON_SHOTGUN},
	{POWERUP_WEAPON, WEAPON_GRENADE},
	{POWERUP_HEALTH, 0},
	{POWERUP_ARMOR, 0},
};

int CGameControllerTKT::OnPickup(class CPickup *pPickup, class CCharacter *pChar, struct SPickupSound *pSound)
{
	if(!pPickup || !pChar)
		return -1;

	// use a stack default when no sound output is requested
	SPickupSound DefaultSound;
	DefaultSound.m_Global = false;
	DefaultSound.m_Sound = -1;
	if(!pSound)
		pSound = &DefaultSound;

	int Type = pPickup->GetType();
	int Subtype = pPickup->GetSubtype();
	pSound->m_Global = false;

	switch(Type)
	{
	case POWERUP_WEAPON:
		// tkt players hold a single weapon: drop everything first, then
		// pick up the new one (1 round) and force-switch to it
		if(Subtype == WEAPON_SHOTGUN)
		{
			pChar->RemoveWeapons();
			if(!pChar->GiveWeapon(WEAPON_SHOTGUN, WEAPON_ID_SHOTGUN, 1))
				return -1;
			CWeapon *pWeapon = pChar->GetWeapon(WEAPON_SHOTGUN);
			if(pWeapon)
				pWeapon->SetMaxAmmo(1);
			pChar->SetWeaponSlot(WEAPON_SHOTGUN, true);
			pSound->m_Sound = SOUND_PICKUP_SHOTGUN;
			GameServer()->SendWeaponPickup(pChar->GetPlayer()->GetCID(), Subtype);
		}
		else if(Subtype == WEAPON_GRENADE)
		{
			pChar->RemoveWeapons();
			if(!pChar->GiveWeapon(WEAPON_GRENADE, WEAPON_ID_GRENADE, 1))
				return -1;
			CWeapon *pWeapon = pChar->GetWeapon(WEAPON_GRENADE);
			if(pWeapon)
				pWeapon->SetMaxAmmo(1);
			pChar->SetWeaponSlot(WEAPON_GRENADE, true);
			pSound->m_Sound = SOUND_PICKUP_GRENADE;
			GameServer()->SendWeaponPickup(pChar->GetPlayer()->GetCID(), Subtype);
		}
		else
			return -1;
		break;

	case POWERUP_HEALTH:
		if(pChar->Health() >= m_PlayerHealthTKT)
			return -1;
		pChar->IncreaseHealth(1);
		pSound->m_Sound = SOUND_PICKUP_HEALTH;
		break;

	case POWERUP_ARMOR:
		if(pChar->Armor() >= m_PlayerArmorTKT)
			return -1;
		pChar->IncreaseArmor(1);
		pSound->m_Sound = SOUND_PICKUP_ARMOR;
		break;

	default:
		return -1;
	}

	// the pickup is destroyed, remove it from the tracked entities
	for(auto Iter = m_ItemEntities.begin(); Iter != m_ItemEntities.end(); ++Iter)
	{
		if(*Iter == pPickup)
		{
			m_ItemEntities.erase(Iter);
			break;
		}
	}

	return -2; // consume the pickup, it doesn't respawn on its own
}

void CGameControllerTKT::DoWincheckRound()
{
	// check for time based win
	if(!m_SuddenDeath && m_GameInfo.m_TimeLimit > 0 && (Server()->Tick() - m_GameStartTick) >= m_GameInfo.m_TimeLimit * Server()->TickSpeed() * 60)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer *pPlayer = GetPlayerIfInRoom(i);
			if(pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS &&
				(!pPlayer->m_RespawnDisabled ||
					(pPlayer->GetCharacter() && pPlayer->GetCharacter()->IsAlive())))
				pPlayer->m_Score++;
		}

		EndRound();
	}
	else
	{
		// check for survival win: last one standing wins
		CPlayer *pAlivePlayer = 0;
		int AlivePlayerCount = 0;
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer *pPlayer = GetPlayerIfInRoom(i);
			if(pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS &&
				(!pPlayer->m_RespawnDisabled ||
					(pPlayer->GetCharacter() && pPlayer->GetCharacter()->IsAlive())))
			{
				++AlivePlayerCount;
				pAlivePlayer = pPlayer;
			}
		}

		if(AlivePlayerCount == 0) // no winner
		{
			EndRound();
		}
		else if(AlivePlayerCount == 1) // 1 winner
		{
			pAlivePlayer->m_Score++;
			EndRound();
		}
	}

	// the round ends: let the END_ROUND phase play out (scoreboard),
	// the reload is triggered in OnPostTick once the state changes
	if(IsEndRound())
		m_ReloadPending = true;
}

void CGameControllerTKT::DoWincheckMatch()
{
}

void CGameControllerTKT::OnPostTick()
{
	// once the END_ROUND phase (scoreboard) is over, move to the next map:
	// inside a cycle the arena switches via a soft instance reload, the map
	// file stays loaded and the players do not reconnect; when the cycle is
	// over a fresh mega map is generated and the map reloads once
	if(m_ReloadPending && m_GameState != IGS_END_ROUND)
	{
		m_ReloadPending = false;
		if(m_IsMegaMap && m_CycleRound + 1 < m_SubMapCountTKT)
		{
			m_CycleRound++;
			char aCmd[64];
			str_format(aCmd, sizeof(aCmd), "map tkt_%d", m_CycleRound + 1);
			dbg_msg("tkt", "cycle round %d/%d, soft switch to submap %d (no reconnect)", m_CycleRound + 1, m_SubMapCountTKT, m_CycleRound + 1);
			InstanceConsole()->ExecuteLine(aCmd);
		}
		else
		{
			GenerateMegaTKTMapForPlayers();
			dbg_msg("tkt", "cycle complete (%d maps), reloading mega map", m_SubMapCountTKT);
			GameServer()->Console()->ExecuteLine(m_IsMegaMap ? "reload" : "change_map tkt");
		}
	}

	// tkt players hold a single weapon: as soon as the active weapon runs
	// out of ammo, drop it and fall back to the pistol
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		CPlayer *pPlayer = GetPlayerIfInRoom(i);
		if(!pPlayer)
			continue;
		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;
		CWeapon *pWeapon = pChr->GetWeapon(pChr->GetActiveWeapon());
		if(!pWeapon || pWeapon->GetWeaponID() == WEAPON_ID_PISTOL || pWeapon->GetAmmo() > 0)
			continue;
		pChr->RemoveWeapons();
		pChr->GiveWeapon(WEAPON_GUN, WEAPON_ID_PISTOL, 5);
		CWeapon *pPistol = pChr->GetWeapon(WEAPON_GUN);
		if(pPistol)
			pPistol->SetMaxAmmo(5);
		pChr->SetWeaponSlot(WEAPON_GUN, true);
	}

	if(m_ItemSpawns.empty() || Server()->Tick() < m_NextItemSpawnTick)
		return;

	m_NextItemSpawnTick = Server()->Tick() + 15 * Server()->TickSpeed();

	// random spawn point and a random item from the spawn list
	vec2 Pos = m_ItemSpawns[secure_rand_below(m_ItemSpawns.size())];
	int Item = secure_rand_below(NUM_ITEMS);

	CPickup *pPickup = new CPickup(GameWorld(), s_aItemSpawnList[Item].Type, s_aItemSpawnList[Item].SubType);
	pPickup->m_Pos = Pos;
	m_ItemEntities.push_back(pPickup);
}

void CGameControllerTKT::OnSnap(int SnappingClient)
{
	// suppress the "Round over" broadcast, the scoreboard is shown instead
	if(SnappingClient >= 0 && m_GameState == IGS_END_ROUND && !Server()->IsSixup(SnappingClient))
		GameServer()->SendBroadcast(" ", SnappingClient, false);
}

bool CGameControllerTKT::IsTKMode(IGameController *pController)
{
	return str_comp(pController->GetGameType(), "tkt") == 0;
}

SKTBullet *CGameControllerTKT::CreateBulletData(vec2 Direction, int StartTick, float SpeedFactor, int CallbackID, bool Explode)
{
	SKTBullet *pData = new SKTBullet;
	pData->m_Direction = Direction;
	pData->m_StartTick = StartTick;
	pData->m_BouncesLeft = -1; // initialized from the room config on the first wall hit
	pData->m_SpeedFactor = SpeedFactor;
	pData->m_CallbackID = CallbackID;
	pData->m_Explode = Explode;
	return pData;
}

float CGameControllerTKT::BulletSpeedFactor(int WeaponType, float TuneSpeed)
{
	float ConfigSpeed;
	switch(WeaponType)
	{
	case WEAPON_SHOTGUN:
		ConfigSpeed = m_ShotgunSpeedTKT;
		break;
	case WEAPON_GRENADE:
		ConfigSpeed = m_GrenadeSpeedTKT;
		break;
	default:
		ConfigSpeed = m_GunSpeedTKT;
		break;
	}

	return TuneSpeed > 0.0f ? ConfigSpeed / TuneSpeed : 1.0f;
}

SBulletMode CGameControllerTKT::GetBulletMode(int WeaponType, vec2 Direction, int StartTick, float TuneSpeed)
{
	float SpeedFactor = BulletSpeedFactor(WeaponType, TuneSpeed);
	vec2 BulletDir = Direction * SpeedFactor;
	int CallbackID = WeaponType == WEAPON_SHOTGUN ? TKTBullets::CALLBACK_BOUNCE_TEE : TKTBullets::CALLBACK_BOUNCE_WALL;

	SBulletMode Mode(BulletDir);
	Mode.m_pCallback = TKTBullets::GetCallback(CallbackID);
	Mode.m_pData = CreateBulletData(BulletDir, StartTick, SpeedFactor, CallbackID);
	Mode.m_pDestroyData = DestroyBulletData;
	Mode.m_Bouncy = fabs(SpeedFactor - 1.0f) < 0.01f;
	Mode.m_LifeSpan = round_to_int(GameServer()->Tuning()->m_GunLifetime * Server()->TickSpeed());
	return Mode;
}

void CGameControllerTKT::DestroyBulletData(void *pData)
{
	delete (SKTBullet *)pData;
}

static void GrenadeExplode(CProjectile *pProj, vec2 Pos, vec2 SpawnDir = vec2(0, 0), bool SpawnChain = true)
{
	CGameWorld *pWorld = pProj->GameWorld();
	TKTBullets::SpawnExplosionCluster(pWorld, Pos, 1, pProj->GetOwner(), pProj->GetWeaponID());

	if(!SpawnChain)
		return; // killed on a character, no chain grenade

	SKTBullet *pData = (SKTBullet *)pProj->GetCustomData();

	// chain limit: carry the remaining bounce count, decreased by one
	int BouncesLeft = pData->m_BouncesLeft;
	if(BouncesLeft < 0)
	{
		CGameControllerTKT *pTKT = (CGameControllerTKT *)pProj->Controller();
		BouncesLeft = pTKT->GrenadeBounceLimit();
	}
	BouncesLeft--;
	if(BouncesLeft <= 0)
		return; // chain exhausted, no more grenade

	// spawn a bouncing grenade, continuing in the given direction (the reflected
	// direction on wall hits, the flight direction elsewhere)
	vec2 Dir = length(SpawnDir) > 0.0f ? SpawnDir : pData->m_Direction;
	int LifeSpan = round_to_int(pProj->GameServer()->Tuning()->m_GunLifetime * pProj->Server()->TickSpeed());
	SKTBullet *pNewData = CGameControllerTKT::CreateBulletData(Dir, pProj->Server()->Tick(), pData->m_SpeedFactor, TKTBullets::CALLBACK_BOUNCE_WALL);
	pNewData->m_BouncesLeft = BouncesLeft;
	CProjectile *pBouncer = new CProjectile(
		pWorld,
		WEAPON_GRENADE,
		pProj->GetWeaponID(),
		pProj->GetOwner(),
		Pos + normalize(Dir) * 8.0f,
		Dir,
		pProj->m_Radius,
		LifeSpan,
		TKTBullets::GetCallback(TKTBullets::CALLBACK_BOUNCE_WALL),
		{pNewData, CGameControllerTKT::DestroyBulletData});
	if(fabs(pData->m_SpeedFactor - 1.0f) < 0.01f)
		pBouncer->SetBouncing(3);
}

void TKTBullets::SpawnExplosionCluster(CGameWorld *pWorld, vec2 Pos, int Radius, int Owner, int WeaponID)
{
	const float Spacing = 64.0f; // fixed grid spacing, no tangency
	const int Damage = g_pData->m_Weapons.m_aId[WEAPON_GRENADE].m_Damage;

	auto Explode = [&](vec2 ExplosionPos) {
		pWorld->CreateExplosion(ExplosionPos, Owner, WEAPON_GRENADE, WeaponID, Damage, Owner < 0);
		pWorld->CreateSound(ExplosionPos, SOUND_GRENADE_EXPLODE);
	};

	if(Radius <= 0)
	{
		Explode(Pos);
		return;
	}

	if(Radius == 1)
	{
		// 3 explosions, 120 degrees apart
		for(int i = 0; i < 3; ++i)
		{
			float a = (2.0f * pi * i) / 3.0f;
			Explode(Pos + vec2(cosf(a), sinf(a)) * Spacing);
		}
		return;
	}

	// hexagonal grid: center + rings
	Explode(Pos);
	vec2 V1(Spacing, 0.0f);
	vec2 V2(Spacing * cosf(pi / 3.0f), Spacing * sinf(pi / 3.0f));
	static const int s_Da[6] = {0, -1, -1, 0, 1, 1};
	static const int s_Db[6] = {-1, 0, 1, 1, 0, -1};
	for(int k = 1; k <= Radius; ++k)
	{
		int a = k, b = 0;
		for(int Side = 0; Side < 6; ++Side)
		{
			for(int Step = 0; Step < k; ++Step)
			{
				Explode(Pos + V1 * (float)a + V2 * (float)b);
				a += s_Da[Side];
				b += s_Db[Side];
			}
		}
	}
}

bool CGameControllerTKT::BulletCollide(CProjectile *pProj, vec2 Pos, CCharacter *pHit, bool EndOfLife)
{
	SKTBullet *pData = (SKTBullet *)pProj->GetCustomData();

	// hit a character
	if(pHit)
	{
		if(pHit->GetPlayer()->GetCID() == pProj->GetOwner())
		{
			// before the first bounce the bullet passes through its owner,
			// after bouncing it can hurt it
			if(pData->m_BouncesLeft < 0)
				return false;
		}

		if(pProj->m_Type == WEAPON_GRENADE && pData->m_Explode)
			GrenadeExplode(pProj, Pos, vec2(0, 0), false);
		else
			pHit->TakeDamage(vec2(0, 0), g_pData->m_Weapons.m_aId[pProj->m_Type].m_Damage, pProj->GetOwner(), pProj->m_Type, pProj->GetWeaponID(), false);
		return true;
	}

	// lifespan exhausted
	if(EndOfLife)
	{
		if(pProj->m_Type == WEAPON_GRENADE && pData->m_Explode)
			GrenadeExplode(pProj, Pos);
		return true;
	}

	// wall hit: bounce the bullet
	CGameControllerTKT *pTKT = (CGameControllerTKT *)pProj->Controller();

	// current and previous tick positions, same math as CProjectile::Tick
	IServer *pServer = pProj->Server();
	float TickTime = 1.0f / (float)pServer->TickSpeed();
	float TimeNow = (pServer->Tick() - pData->m_StartTick) * TickTime;
	vec2 CurPos = pProj->GetPos(TimeNow);
	vec2 PrevPos = pProj->GetPos(TimeNow - TickTime);
	vec2 ColPos, NewPos;
	CCollision *pCollision = pProj->GameServer()->Collision();
	if(!pCollision->IntersectLine(PrevPos, CurPos, &ColPos, &NewPos))
	{
		// flew out of the map or math edge case, a grenade explodes here
		if(pProj->m_Type == WEAPON_GRENADE && pData->m_Explode)
			GrenadeExplode(pProj, Pos);
		return true;
	}

	// detect the wall orientation: compare the tiles of the impact point
	// and the last free point to find the boundary that was crossed,
	// giving an exact incidence=reflection bounce on grid walls
	int SolidTileX = round_to_int(ColPos.x) / 32;
	int SolidTileY = round_to_int(ColPos.y) / 32;
	int FreeTileX = round_to_int(NewPos.x) / 32;
	int FreeTileY = round_to_int(NewPos.y) / 32;
	bool WallX = SolidTileX != FreeTileX;
	bool WallY = SolidTileY != FreeTileY;
	if(!WallX && !WallY)
	{
		// cannot determine the wall, a grenade explodes here
		if(pProj->m_Type == WEAPON_GRENADE && pData->m_Explode)
			GrenadeExplode(pProj, Pos);
		return true;
	}

	vec2 NewDir = pData->m_Direction;
	if(WallX)
		NewDir.x = -NewDir.x;
	if(WallY)
		NewDir.y = -NewDir.y;

	// grenades explode on every wall contact, the chain grenade bounces off the wall
	if(pProj->m_Type == WEAPON_GRENADE && pData->m_Explode)
	{
		GrenadeExplode(pProj, Pos, NewDir);
		return true;
	}

	if(!SpawnBouncedBullet(pProj, NewDir, NewPos + NewDir * 4.0f, pTKT) && pProj->m_Type == WEAPON_GRENADE && pData->m_Explode)
		GrenadeExplode(pProj, Pos, NewDir);

	return true; // destroy the old projectile
}

bool CGameControllerTKT::BulletCollideTee(CProjectile *pProj, vec2 Pos, CCharacter *pHit, bool EndOfLife)
{
	// walls and end of life behave like the wall callback
	if(!pHit)
		return BulletCollide(pProj, Pos, nullptr, EndOfLife);

	SKTBullet *pData = (SKTBullet *)pProj->GetCustomData();
	if(pHit->GetPlayer()->GetCID() == pProj->GetOwner())
	{
		// before the first bounce the bullet passes through its owner,
		// after bouncing it can hurt it
		if(pData->m_BouncesLeft < 0)
			return false;

		pHit->TakeDamage(vec2(0, 0), g_pData->m_Weapons.m_aId[pProj->m_Type].m_Damage, pProj->GetOwner(), pProj->m_Type, pProj->GetWeaponID(), false);
		return true;
	}

	// bounce off the tee along the surface normal, no damage
	vec2 ToHit = Pos - pHit->m_Pos;
	float Dist = length(ToHit);
	if(Dist < 0.001f)
		return true;

	vec2 Normal = ToHit / Dist;
	vec2 NewDir = pData->m_Direction - Normal * (2.0f * dot(pData->m_Direction, Normal));

	// spawn just outside the tee so it doesn't re-collide immediately
	vec2 NewPos = pHit->m_Pos + Normal * (pHit->GetProximityRadius() + 4.0f);
	SpawnBouncedBullet(pProj, NewDir, NewPos, (CGameControllerTKT *)pProj->Controller());
	return true; // destroy the old projectile
}

bool CGameControllerTKT::SpawnBouncedBullet(CProjectile *pProj, vec2 NewDir, vec2 NewPos, CGameControllerTKT *pTKT)
{
	SKTBullet *pData = (SKTBullet *)pProj->GetCustomData();

	int BouncesLeft = pData->m_BouncesLeft;
	if(BouncesLeft < 0)
	{
		switch(pProj->m_Type)
		{
		case WEAPON_SHOTGUN:
			BouncesLeft = pTKT->m_ShotgunBounceTKT;
			break;
		case WEAPON_GRENADE:
			BouncesLeft = pTKT->m_GrenadeBounceTKT;
			break;
		default:
			BouncesLeft = pTKT->m_GunBounceTKT;
			break;
		}
	}

	if(BouncesLeft <= 0)
		return false;

	// spawn the bounced bullet, transferring the members with the bounce count decreased
	SKTBullet *pNewData = new SKTBullet(*pData);
	pNewData->m_BouncesLeft = BouncesLeft - 1;
	pNewData->m_Direction = NewDir;
	pNewData->m_StartTick = pProj->Server()->Tick();

	CProjectile *pNew = new CProjectile(
		pProj->GameWorld(),
		pProj->m_Type,
		pProj->GetWeaponID(),
		pProj->GetOwner(),
		NewPos,
		NewDir,
		pProj->m_Radius,
		pProj->m_LifeSpan,
		TKTBullets::GetCallback(pData->m_CallbackID),
		{pNewData, DestroyBulletData});

	// snap as a bouncy projectile so clients predict the bounce,
	// only if the bullet speed matches the tuning speed (the client
	// derives the speed from the tune for bouncy projectiles)
	if(fabs(pData->m_SpeedFactor - 1.0f) < 0.01f)
		pNew->SetBouncing(3);

	return true;
}
