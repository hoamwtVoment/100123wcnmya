#include "grenade.h"
#include <game/generated/server_data.h>
#include <game/server/entities/projectile.h>

CGrenade::CGrenade(CCharacter *pOwnerChar) :
	CWeapon(pOwnerChar)
{
	m_MaxAmmo = g_pData->m_Weapons.m_aId[WEAPON_GRENADE].m_Maxammo;
	m_AmmoRegenTime = g_pData->m_Weapons.m_aId[WEAPON_GRENADE].m_Ammoregentime;
	m_FireDelay = g_pData->m_Weapons.m_aId[WEAPON_GRENADE].m_Firedelay;
	m_FullAuto = true;
}

bool CGrenade::GrenadeCollide(CProjectile *pProj, vec2 Pos, CCharacter *pHit, bool EndOfLife)
{
	if(pHit && pHit->GetPlayer()->GetCID() == pProj->GetOwner())
		return false;

	pProj->GameWorld()->CreateExplosion(Pos, pProj->GetOwner(), WEAPON_GRENADE, pProj->GetWeaponID(), g_pData->m_Weapons.m_aId[WEAPON_GRENADE].m_Damage, pProj->GetOwner() < 0);
	pProj->GameWorld()->CreateSound(Pos, SOUND_GRENADE_EXPLODE);

	return true;
}

void CGrenade::Fire(vec2 Direction)
{
	int ClientID = Character()->GetPlayer()->GetCID();
	int Lifetime = Character()->CurrentTuning()->m_GrenadeLifetime * Server()->TickSpeed();

	vec2 ProjStartPos = Pos() + Direction * GetProximityRadius() * 0.75f;

	SBulletMode Mode = Character()->Controller()->GetBulletMode(WEAPON_GRENADE, Direction, Server()->Tick(), Character()->CurrentTuning()->m_GrenadeSpeed);
	if(Mode.m_LifeSpan > 0)
		Lifetime = Mode.m_LifeSpan;

	CProjectile *pProj = new CProjectile(
		GameWorld(),
		WEAPON_GRENADE, // Type
		GetWeaponID(), // WeaponID
		ClientID, // Owner
		ProjStartPos, // Pos
		Mode.m_Direction, // Dir
		6.0f, // Radius
		Lifetime, // Span
		Mode.m_pCallback ? Mode.m_pCallback : GrenadeCollide,
		{Mode.m_pData, Mode.m_pDestroyData});

	if(Mode.m_Bouncy)
		pProj->SetBouncing(3);

	// pack the Projectile and send it to the client Directly,
	// skipped for mode-owned bullets to avoid ghost copies on bounce
	if(!Mode.m_pCallback)
	{
		CNetObj_Projectile p;
		pProj->FillInfo(&p);

		CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
		Msg.AddInt(1);
		for(unsigned i = 0; i < sizeof(CNetObj_Projectile) / sizeof(int); i++)
			Msg.AddInt(((int *)&p)[i]);

		Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
	}

	GameWorld()->CreateSound(Character()->m_Pos, SOUND_GRENADE_FIRE);
}