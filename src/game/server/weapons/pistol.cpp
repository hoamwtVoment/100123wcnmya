#include "pistol.h"
#include <game/generated/server_data.h>
#include <game/server/entities/projectile.h>

CPistol::CPistol(CCharacter *pOwnerChar) :
	CWeapon(pOwnerChar)
{
	m_MaxAmmo = g_pData->m_Weapons.m_aId[WEAPON_GUN].m_Maxammo;
	m_AmmoRegenTime = g_pData->m_Weapons.m_aId[WEAPON_GUN].m_Ammoregentime;
	m_FireDelay = g_pData->m_Weapons.m_aId[WEAPON_GUN].m_Firedelay;
}

bool CPistol::BulletCollide(CProjectile *pProj, vec2 Pos, CCharacter *pHit, bool EndOfLife)
{
	if(pHit)
	{
		if(pHit->GetPlayer()->GetCID() == pProj->GetOwner())
			return false;

		pHit->TakeDamage(vec2(0, 0), g_pData->m_Weapons.m_aId[WEAPON_GUN].m_Damage, pProj->GetOwner(), WEAPON_GUN, pProj->GetWeaponID(), false);
	}

	return true;
}

void CPistol::Fire(vec2 Direction)
{
	int ClientID = Character()->GetPlayer()->GetCID();
	int Lifetime = Character()->CurrentTuning()->m_GunLifetime * Server()->TickSpeed();

	vec2 ProjStartPos = Pos() + Direction * GetProximityRadius() * 0.75f;

	SBulletMode Mode = Character()->Controller()->GetBulletMode(WEAPON_GUN, Direction, Server()->Tick(), Character()->CurrentTuning()->m_GunSpeed);
	if(Mode.m_LifeSpan > 0)
		Lifetime = Mode.m_LifeSpan;

	CProjectile *pProj = new CProjectile(
		GameWorld(),
		WEAPON_GUN, // Type
		GetWeaponID(), // WeaponID
		ClientID, // Owner
		ProjStartPos, // Pos
		Mode.m_Direction, // Dir
		6.0f, // Radius
		Lifetime, // Span
		Mode.m_pCallback ? Mode.m_pCallback : BulletCollide,
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

	GameWorld()->CreateSound(Character()->m_Pos, SOUND_GUN_FIRE);
}