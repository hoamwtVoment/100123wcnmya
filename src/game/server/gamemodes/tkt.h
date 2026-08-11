/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_GAMEMODES_TKT_H
#define GAME_SERVER_GAMEMODES_TKT_H

#include <map>
#include <vector>

#include <game/server/gamecontroller.h>

class CCharacter;
class CEntity;
class CProjectile;

// per-bullet state, transferred to the bounced bullet on wall hits
struct SKTBullet
{
	vec2 m_Direction;
	int m_StartTick;
	int m_BouncesLeft;
	float m_SpeedFactor;
	int m_CallbackID;
	bool m_Explode; // grenades: explode on contact, false = bounce like a gun
};

class CGameControllerTKT : public IGameController
{
private:
	int m_RespawnDelayTKT;
	int m_RoundTimeTKT;
	int m_InvincibleTicksTKT;
	int m_PlayerHealthTKT;
	int m_PlayerArmorTKT;
	int m_GunSpeedTKT;
	int m_ShotgunSpeedTKT;
	int m_GrenadeSpeedTKT;
	int m_GunBounceTKT;
	int m_ShotgunBounceTKT;
	int m_GrenadeBounceTKT;
	// item spawn points from red flag tiles, and the periodic item spawn state
	std::vector<vec2> m_ItemSpawns;
	std::vector<CEntity *> m_ItemEntities;
	int m_NextItemSpawnTick;
	bool m_ReloadPending;
	// cycle of submaps: m_SubMapCountTKT arenas are pre-generated into one
	// mega map, each round plays one of them without reconnecting
	int m_SubMapCountTKT;
	int m_CycleRound; // 0-based round index inside the current cycle
	bool m_IsMegaMap;
	void GenerateMegaTKTMapForPlayers();

public:
	CGameControllerTKT();

	// lifecycle
	virtual void OnInit() override;
	virtual void OnControllerStart() override;
	virtual void OnGameStart(bool IsRound) override;
	virtual void OnWorldReset() override;
	virtual void OnPlayerJoin(class CPlayer *pPlayer) override;
	virtual void OnPlayerLeave(class CPlayer *pPlayer) override;

	// gameplay
	virtual bool OnPlayerTryRespawn(class CPlayer *pPlayer, vec2 Pos) override;
	virtual bool CanChangeTeam(class CPlayer *pPlayer, int JoinTeam) const override;
	virtual void OnCharacterSpawn(class CCharacter *pChr) override;
	virtual int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon) override;
	virtual int OnCharacterTakeDamage(class CCharacter *pChr, vec2 &Force, int &Dmg, int From, int WeaponType, int WeaponID, bool IsExplosion) override;
	virtual bool OnCharacterTile(class CCharacter *pChr, int MapIndex) override;
	virtual bool OnEntity(int Index, vec2 Pos, int Layer, int Flags, int Number = 0) override;
	virtual int OnPickup(class CPickup *pPickup, class CCharacter *pChar, struct SPickupSound *pSound) override;

	// round / match flow
	virtual void DoWincheckRound() override;
	virtual void DoWincheckMatch() override;
	virtual void OnPostTick() override;
	virtual void OnSnap(int SnappingClient) override;

	// bullet system: shared impact callback for all projectile weapons
	virtual SBulletMode GetBulletMode(int WeaponType, vec2 Direction, int StartTick, float TuneSpeed) override;
	int GrenadeBounceLimit() { return m_GrenadeBounceTKT; }
	static bool IsTKMode(class IGameController *pController);
	float BulletSpeedFactor(int WeaponType, float TuneSpeed);
	static SKTBullet *CreateBulletData(vec2 Direction, int StartTick, float SpeedFactor, int CallbackID, bool Explode = true);
	static bool BulletCollide(class CProjectile *pProj, vec2 Pos, class CCharacter *pHit, bool EndOfLife);
	static bool BulletCollideTee(class CProjectile *pProj, vec2 Pos, class CCharacter *pHit, bool EndOfLife);
	static bool SpawnBouncedBullet(class CProjectile *pProj, vec2 NewDir, vec2 NewPos, class CGameControllerTKT *pTKT);
	static void DestroyBulletData(void *pData);
};

namespace TKTBullets
{
	enum
	{
		CALLBACK_BOUNCE_WALL = 0,
		CALLBACK_BOUNCE_TEE,
		NUM_CALLBACKS,
	};

	typedef bool (*FCallback)(class CProjectile *, vec2, class CCharacter *, bool);

	// enum -> callback map table
	inline FCallback GetCallback(int ID)
	{
		static const std::map<int, FCallback> s_Callbacks = {
			{CALLBACK_BOUNCE_WALL, CGameControllerTKT::BulletCollide},
			{CALLBACK_BOUNCE_TEE, CGameControllerTKT::BulletCollideTee},
		};
		auto Iter = s_Callbacks.find(ID);
		return Iter != s_Callbacks.end() ? Iter->second : CGameControllerTKT::BulletCollide;
	}

	// explosion cluster: hexagonal packing, explosion ranges are tangent to their neighbors
	// radius 0: single explosion, radius 1: 3 tangent explosions, radius 2+: hexagon of 7, 19, ...
	void SpawnExplosionCluster(class CGameWorld *pWorld, vec2 Pos, int Radius, int Owner, int WeaponID);
}

#endif
