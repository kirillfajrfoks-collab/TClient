/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <algorithm>

#include <base/io.h>
#include <base/math.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/storage.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

CControls::CControls()
{
	mem_zero(&m_aLastData, sizeof(m_aLastData));
	std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
}

void CControls::OnReset()
{
	ResetInput(0);
	ResetInput(1);

	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;

	m_LastSendTime = 0;
	m_AvoidFreezeMessageTick = 0;
	m_AvoidFreezeHooked = false;
	m_AvoidFreezeHookUntilTick = 0;
	m_AvoidFreezeHookCooldownTick = 0;
	m_LastEmoteSpamTime = 0;
	m_EmoteSpamIndex = 0;
	m_AimCorrectionPendingTick = -1;
	m_AimCorrectionPendingClientId = -1;
	m_AimCorrectionPendingLateralMiss = 0.0f;
	m_LastAimCorrectionLogTick = -1;
	m_LastAimCorrectionLogClientId = -1;
}

void CControls::ResetInput(int Dummy)
{
	m_aLastData[Dummy].m_Direction = 0;
	// simulate releasing the fire button
	if((m_aLastData[Dummy].m_Fire & 1) != 0)
		m_aLastData[Dummy].m_Fire++;
	m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
	m_aLastData[Dummy].m_Jump = 0;
	m_aInputData[Dummy] = m_aLastData[Dummy];

	m_aInputDirectionLeft[Dummy] = 0;
	m_aInputDirectionRight[Dummy] = 0;
}

void CControls::OnPlayerDeath()
{
	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;
}

struct CInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	*pState->m_apVariables[g_Config.m_ClDummy] = pResult->GetInteger(0);
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int m_Value;
};

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		*pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
	}
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	ConKeyInputCounter(pResult, pSet);
	pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1]}};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1]}};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump}};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Hook, &m_aInputData[1].m_Hook}};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire}};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1]}};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		m_aAmmoCount[maximum(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

bool CControls::IsFreezeTile(vec2 Pos)
{
	const int Index = Collision()->GetPureMapIndex(Pos.x, Pos.y);
	const int Tile = Collision()->GetTileIndex(Index);
	const int FrontTile = Collision()->GetFrontTileIndex(Index);
	return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE ||
	       FrontTile == TILE_FREEZE || FrontTile == TILE_DFREEZE || FrontTile == TILE_LFREEZE;
}

bool CControls::IsSingleFreezeTile(vec2 Pos)
{
	if(!IsFreezeTile(Pos))
		return false;

	static const vec2 s_aOffsets[] = {vec2(-32.0f, 0.0f), vec2(32.0f, 0.0f), vec2(0.0f, -32.0f), vec2(0.0f, 32.0f)};
	for(const vec2 &Offset : s_aOffsets)
	{
		if(IsFreezeTile(Pos + Offset))
			return false;
	}
	return true;
}

bool CControls::PiFuncCanAimClient(int ClientId) const
{
	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy] >= 0 ? GameClient()->m_aLocalIds[g_Config.m_ClDummy] : GameClient()->m_Snap.m_LocalClientId;
	if(ClientId == LocalId || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
		return false;

	if(!g_Config.m_TcPiFuncNotAimTeam)
		return true;

	if(LocalId < 0)
		return false;

	const auto &WarGroups = GameClient()->m_WarList.GetWarData(ClientId).m_WarGroupMatches;
	return WarGroups.size() <= 2 || !WarGroups[2];
}

bool CControls::IsClientFrozen(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
		return false;
	return GameClient()->m_aClients[ClientId].m_FreezeEnd != 0 || GameClient()->m_aClients[ClientId].m_DeepFrozen || GameClient()->m_aClients[ClientId].m_LiveFrozen;
}

void CControls::HammerTarget(vec2 Pos, vec2 TargetPos)
{
	const vec2 Aim = TargetPos - Pos;
	m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = WEAPON_HAMMER + 1;
	m_aInputData[g_Config.m_ClDummy].m_TargetX = round_to_int(Aim.x);
	m_aInputData[g_Config.m_ClDummy].m_TargetY = round_to_int(Aim.y);
	if(GameClient()->m_PredictedChar.m_ActiveWeapon != WEAPON_HAMMER && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER)
		return;
	if((m_aInputData[g_Config.m_ClDummy].m_Fire & 1) == 0)
		m_aInputData[g_Config.m_ClDummy].m_Fire++;
	m_aInputData[g_Config.m_ClDummy].m_Fire &= INPUT_STATE_MASK;
}

void CControls::LogAimCorrection(int ClientId, vec2 Pos, vec2 TargetPos, vec2 TargetVel, vec2 OldAim, vec2 NewAim, float TravelTicks, float LateralMiss)
{
	if(!g_Config.m_TcAimCorrectionLog)
		return;
	const int Tick = Client()->GameTick(g_Config.m_ClDummy);
	if(ClientId == m_LastAimCorrectionLogClientId && Tick - m_LastAimCorrectionLogTick < 6)
		return;
	m_LastAimCorrectionLogTick = Tick;
	m_LastAimCorrectionLogClientId = ClientId;

	Storage()->CreateFolder("pifunc", IStorage::TYPE_SAVE);
	bool WriteHeader = true;
	IOHANDLE ExistingFile = Storage()->OpenFile("pifunc/aim_correction.csv", IOFLAG_READ, IStorage::TYPE_SAVE);
	if(ExistingFile)
	{
		WriteHeader = io_length(ExistingFile) == 0;
		io_close(ExistingFile);
	}

	IOHANDLE File = Storage()->OpenFile("pifunc/aim_correction.csv", IOFLAG_APPEND, IStorage::TYPE_SAVE);
	if(!File)
		return;

	if(WriteHeader && !m_AimCorrectionLogHeaderWritten)
	{
		static const char s_aHeader[] = "tick,client_id,pos_x,pos_y,target_x,target_y,target_vx,target_vy,old_aim_x,old_aim_y,new_aim_x,new_aim_y,travel_ticks,lateral_miss\n";
		io_write(File, s_aHeader, sizeof(s_aHeader) - 1);
	}
	m_AimCorrectionLogHeaderWritten = true;

	char aLine[512];
	str_format(aLine, sizeof(aLine), "%d,%d,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f\n",
		Client()->GameTick(g_Config.m_ClDummy), ClientId, Pos.x, Pos.y, TargetPos.x, TargetPos.y, TargetVel.x, TargetVel.y, OldAim.x, OldAim.y, NewAim.x, NewAim.y, TravelTicks, LateralMiss);
	io_write(File, aLine, str_length(aLine));
	io_close(File);

	m_AimCorrectionPendingTick = Tick;
	m_AimCorrectionPendingClientId = ClientId;
	m_AimCorrectionPendingLateralMiss = LateralMiss;
}

void CControls::LogAimCorrectionResult()
{
	if(!g_Config.m_TcAimCorrectionLog || m_AimCorrectionPendingTick < 0 || GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter)
		return;

	const int CurrentTick = Client()->GameTick(g_Config.m_ClDummy);
	const int Age = CurrentTick - m_AimCorrectionPendingTick;
	if(Age < 2)
		return;
	if(Age > 18)
	{
		m_AimCorrectionPendingTick = -1;
		return;
	}

	const int HookedPlayer = GameClient()->m_PredictedChar.HookedPlayer();
	if(HookedPlayer == -1 && Age < 10)
		return;

	Storage()->CreateFolder("pifunc", IStorage::TYPE_SAVE);
	bool WriteHeader = true;
	IOHANDLE ExistingFile = Storage()->OpenFile("pifunc/aim_correction_result.csv", IOFLAG_READ, IStorage::TYPE_SAVE);
	if(ExistingFile)
	{
		WriteHeader = io_length(ExistingFile) == 0;
		io_close(ExistingFile);
	}

	IOHANDLE File = Storage()->OpenFile("pifunc/aim_correction_result.csv", IOFLAG_APPEND, IStorage::TYPE_SAVE);
	if(!File)
	{
		m_AimCorrectionPendingTick = -1;
		return;
	}

	if(WriteHeader && !m_AimCorrectionResultLogHeaderWritten)
	{
		static const char s_aHeader[] = "correction_tick,result_tick,target_client_id,hooked_client_id,success,age_ticks,lateral_miss\n";
		io_write(File, s_aHeader, sizeof(s_aHeader) - 1);
	}
	m_AimCorrectionResultLogHeaderWritten = true;

	char aLine[256];
	str_format(aLine, sizeof(aLine), "%d,%d,%d,%d,%d,%d,%.3f\n",
		m_AimCorrectionPendingTick, CurrentTick, m_AimCorrectionPendingClientId, HookedPlayer, HookedPlayer == m_AimCorrectionPendingClientId, Age, m_AimCorrectionPendingLateralMiss);
	io_write(File, aLine, str_length(aLine));
	io_close(File);

	m_AimCorrectionPendingTick = -1;
}

void CControls::AvoidFreeze()
{
	m_AvoidFreezeJumped = false;
	const int Tick = Client()->GameTick(g_Config.m_ClDummy);
	const auto ReleaseAvoidHook = [&]() {
		if(m_AvoidFreezeHooked)
		{
			m_aInputData[g_Config.m_ClDummy].m_Hook = 0;
			m_AvoidFreezeHooked = false;
			m_AvoidFreezeHookCooldownTick = Tick + 6;
		}
	};

	if(!g_Config.m_TcAvoidFreeze || GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter)
	{
		ReleaseAvoidHook();
		return;
	}

	const CCharacterCore &Char = GameClient()->m_PredictedChar;
	if(Char.m_IsInFreeze)
	{
		ReleaseAvoidHook();
		return;
	}

	if(m_AvoidFreezeHooked && Tick > m_AvoidFreezeHookUntilTick)
		ReleaseAvoidHook();

	const vec2 Pos = Char.m_Pos;
	const vec2 Vel = Char.m_Vel;
	const bool HookedByPlayer = !Char.m_AttachedPlayers.empty();
	const bool ManualDirection = m_aInputDirectionLeft[g_Config.m_ClDummy] || m_aInputDirectionRight[g_Config.m_ClDummy];
	const int LookAheadTicks = HookedByPlayer ? 14 : 8;

	const auto IsDangerous = [&](vec2 CheckPos) {
		static const vec2 s_aHitboxPoints[] = {
			vec2(-14.0f, -14.0f), vec2(14.0f, -14.0f), vec2(-14.0f, 14.0f), vec2(14.0f, 14.0f),
			vec2(0.0f, -18.0f), vec2(0.0f, 18.0f), vec2(-18.0f, 0.0f), vec2(18.0f, 0.0f)};
		for(const vec2 &Point : s_aHitboxPoints)
		{
			if(IsFreezeTile(CheckPos + Point))
				return true;
		}
		return false;
	};

	const auto DangerScore = [&](int Direction) {
		int Score = 0;
		for(int Tick = 1; Tick <= LookAheadTicks; ++Tick)
		{
			const vec2 CheckPos = Pos + Vel * Tick + vec2(Direction * 6.0f * Tick, 0.0f);
			if(IsDangerous(CheckPos))
				Score += (LookAheadTicks - Tick + 1) * (HookedByPlayer ? 3 : 1);
		}
		return Score;
	};

	const int CurrentScore = DangerScore(0);
	if(CurrentScore == 0 && !HookedByPlayer)
	{
		ReleaseAvoidHook();
		return;
	}

	int AvoidDir = 0;
	if(!ManualDirection)
	{
		const int LeftScore = DangerScore(-1);
		const int RightScore = DangerScore(1);
		if(LeftScore < CurrentScore || RightScore < CurrentScore)
			AvoidDir = LeftScore < RightScore ? -1 : 1;
	}

	bool FreezeAbove = false;
	bool FreezeBelow = false;
	for(int Tick = 1; Tick <= LookAheadTicks; ++Tick)
	{
		const vec2 CheckPos = Pos + Vel * Tick;
		FreezeAbove = FreezeAbove || IsFreezeTile(CheckPos + vec2(-12.0f, -22.0f)) || IsFreezeTile(CheckPos + vec2(12.0f, -22.0f));
		FreezeBelow = FreezeBelow || IsFreezeTile(CheckPos + vec2(-12.0f, 22.0f)) || IsFreezeTile(CheckPos + vec2(12.0f, 22.0f));
	}

	if(AvoidDir == 0 && (!HookedByPlayer || (!FreezeAbove && !FreezeBelow)) && CurrentScore == 0)
	{
		ReleaseAvoidHook();
		return;
	}

	if(AvoidDir != 0)
		m_aInputData[g_Config.m_ClDummy].m_Direction = AvoidDir;
	if(HookedByPlayer && FreezeBelow)
	{
		m_aInputData[g_Config.m_ClDummy].m_Jump = 1;
		m_AvoidFreezeJumped = true;
	}
	if(HookedByPlayer && FreezeAbove && Tick >= m_AvoidFreezeHookCooldownTick)
	{
		m_aInputData[g_Config.m_ClDummy].m_Hook = 1;
		m_aInputData[g_Config.m_ClDummy].m_TargetX = 0;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = 100;
		if(!m_AvoidFreezeHooked)
		{
			m_AvoidFreezeHooked = true;
			m_AvoidFreezeHookUntilTick = Tick + g_Config.m_TcAvoidFreezeHookTicks;
		}
	}
	else if(m_AvoidFreezeHooked && !FreezeAbove)
		ReleaseAvoidHook();
	m_AvoidFreezeMessageTick = Tick + 10;
}

void CControls::ForgiveHook()
{
	if(g_Config.m_TcForgivableHook <= 0 || !m_aInputData[g_Config.m_ClDummy].m_Hook || GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter)
		return;
	if(m_aLastData[g_Config.m_ClDummy].m_Hook || !GameClient()->m_PredictedChar.m_AttachedPlayers.empty())
		return;

	const vec2 Pos = GameClient()->m_PredictedChar.m_Pos;
	const float HookLength = (float)GameClient()->m_aTuning[g_Config.m_ClDummy].m_HookLength;
	const float HookFireSpeed = maximum(1.0f, (float)GameClient()->m_aTuning[g_Config.m_ClDummy].m_HookFireSpeed);
	vec2 Target = vec2(m_aInputData[g_Config.m_ClDummy].m_TargetX, m_aInputData[g_Config.m_ClDummy].m_TargetY);
	if(Target == vec2(0.0f, 0.0f))
		return;
	Target = normalize(Target);

	float BestLateralMiss = 0.0f;
	float SecondBestLateralMiss = 0.0f;
	int ClosestClientId = -1;
	float ClosestTravelTicks = 0.0f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!PiFuncCanAimClient(ClientId))
			continue;

		const CNetObj_Character &Char = GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur;
		const vec2 OtherPos = vec2(Char.m_X, Char.m_Y);
		const vec2 OtherVel = vec2(Char.m_VelX / 256.0f, Char.m_VelY / 256.0f);
		const vec2 ToOther = OtherPos - Pos;
		const float DistanceToTarget = length(ToOther);
		if(DistanceToTarget > HookLength)
			continue;
		const float DistanceAlongHook = dot(ToOther, Target);
		if(DistanceToTarget <= 0.0f || DistanceAlongHook <= 0.0f || DistanceAlongHook > HookLength)
			continue;

		const float HookTicksToTarget = DistanceToTarget / HookFireSpeed;
		const float LeadTicks = std::clamp(HookTicksToTarget * 0.55f, 0.0f, 4.0f);
		const vec2 PredictedOtherPos = OtherPos + OtherVel * LeadTicks;

		const vec2 PredictedToOther = PredictedOtherPos - Pos;
		const float PredictedDistanceAlongHook = dot(PredictedToOther, Target);
		if(PredictedDistanceAlongHook <= 0.0f || PredictedDistanceAlongHook > HookLength)
			continue;
		const vec2 ClosestPoint = Pos + Target * PredictedDistanceAlongHook;
		const float VanillaRadius = CCharacterCore::PhysicalSize() + 2.0f;
		const float LateralMiss = distance(PredictedOtherPos, ClosestPoint);
		if(LateralMiss <= VanillaRadius)
			continue;

		const float ForgivableRadius = std::min((float)std::tan(g_Config.m_TcForgivableHook * pi / 180.0f) * DistanceToTarget, 64.0f);
		vec2 CollisionPos;
		int TeleNr = 0;
		const vec2 Aim = PredictedOtherPos - Pos;
		const vec2 AimDir = normalize(Aim);
		const vec2 HookStart = Pos + AimDir * CCharacterCore::PhysicalSize() * 1.5f;
		if(length(Aim) > HookLength || Collision()->IntersectLineTeleHook(HookStart, PredictedOtherPos, &CollisionPos, nullptr, &TeleNr))
			continue;

		if(LateralMiss < VanillaRadius + ForgivableRadius)
		{
			if(ClosestClientId == -1 || LateralMiss < BestLateralMiss)
			{
				SecondBestLateralMiss = BestLateralMiss;
				ClosestClientId = ClientId;
				BestLateralMiss = LateralMiss;
				ClosestTravelTicks = LeadTicks;
			}
			else if(SecondBestLateralMiss == 0.0f || LateralMiss < SecondBestLateralMiss)
				SecondBestLateralMiss = LateralMiss;
		}
	}

	if(ClosestClientId == -1)
		return;
	if(SecondBestLateralMiss > 0.0f && SecondBestLateralMiss - BestLateralMiss < 18.0f)
		return;

	const CNetObj_Character &Char = GameClient()->m_Snap.m_aCharacters[ClosestClientId].m_Cur;
	const vec2 OtherPos = vec2(Char.m_X, Char.m_Y);
	const vec2 OtherVel = vec2(Char.m_VelX / 256.0f, Char.m_VelY / 256.0f);
	const float HookTicksToTarget = distance(Pos, OtherPos) / HookFireSpeed;
	const float LeadTicks = std::clamp(HookTicksToTarget * 0.55f, 0.0f, 4.0f);
	const vec2 PredictedOtherPos = OtherPos + OtherVel * LeadTicks;
	const vec2 Aim = PredictedOtherPos - Pos;
	if(length(Aim) <= 0.0f || length(Aim) > HookLength)
		return;
	const float Dot = std::clamp(dot(normalize(Aim), Target), -1.0f, 1.0f);
	if(std::acos(Dot) > g_Config.m_TcForgivableHook * pi / 180.0f)
		return;
	const vec2 OldAim = vec2(m_aInputData[g_Config.m_ClDummy].m_TargetX, m_aInputData[g_Config.m_ClDummy].m_TargetY);
	const float OldLength = maximum(1.0f, length(OldAim));
	const vec2 OldDir = normalize(OldAim);
	const vec2 AimDir = normalize(Aim);
	const float AssistStrength = std::clamp((BestLateralMiss - CCharacterCore::PhysicalSize()) / 56.0f, 0.25f, 0.72f);
	const vec2 CorrectedDir = normalize(OldDir * (1.0f - AssistStrength) + AimDir * AssistStrength);
	const vec2 CorrectedAim = CorrectedDir * OldLength;
	m_aInputData[g_Config.m_ClDummy].m_TargetX = round_to_int(CorrectedAim.x);
	m_aInputData[g_Config.m_ClDummy].m_TargetY = round_to_int(CorrectedAim.y);
	LogAimCorrection(ClosestClientId, Pos, OtherPos, OtherVel, OldAim, CorrectedAim, ClosestTravelTicks, BestLateralMiss);
}

void CControls::EmoteSpammer()
{
	if(!g_Config.m_TcEmoteSpammer || GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter)
		return;

	const int64_t Now = time_get();
	const int DelayMs = g_Config.m_TcEmoteSpammerDelay < 100 ? 100 : g_Config.m_TcEmoteSpammerDelay;
	const int64_t Delay = time_freq() * DelayMs / 1000;
	if(m_LastEmoteSpamTime && Now - m_LastEmoteSpamTime < Delay)
		return;

	m_LastEmoteSpamTime = Now;
	const int Emote = m_EmoteSpamIndex ? 12 : 7;
	m_EmoteSpamIndex = !m_EmoteSpamIndex;

	char aCmd[32];
	str_format(aCmd, sizeof(aCmd), "emote %d", Emote);
	Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_UNSPECIFIED);
}

void CControls::AutoLed()
{
	if(!g_Config.m_TcAutoLed || GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter)
		return;

	const vec2 Pos = GameClient()->m_PredictedChar.m_Pos;
	int TargetClientId = -1;
	float ClosestDistance = 0.0f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!PiFuncCanAimClient(ClientId))
			continue;

		const CNetObj_Character &Char = GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur;
		const vec2 OtherPos = vec2(Char.m_X, Char.m_Y);
		const vec2 OtherVel = vec2(Char.m_VelX / 256.0f, Char.m_VelY / 256.0f);
		const float Dist = distance(Pos, OtherPos);
		if(Dist > 140.0f || GameClient()->m_aClients[ClientId].m_HammerHitDisabled)
			continue;

		bool NeedsLed = IsClientFrozen(ClientId);
		for(int Tick = 0; Tick <= 10 && !NeedsLed; ++Tick)
		{
			const vec2 CheckPos = OtherPos + OtherVel * (float)Tick;
			NeedsLed = IsFreezeTile(CheckPos) || IsFreezeTile(CheckPos + vec2(0.0f, 18.0f)) || IsFreezeTile(CheckPos + vec2(-14.0f, 14.0f)) || IsFreezeTile(CheckPos + vec2(14.0f, 14.0f));
		}
		if(!NeedsLed)
			continue;

		if(TargetClientId == -1 || Dist < ClosestDistance)
		{
			TargetClientId = ClientId;
			ClosestDistance = Dist;
		}
	}

	if(TargetClientId == -1)
		return;

	const vec2 OtherPos = vec2(GameClient()->m_Snap.m_aCharacters[TargetClientId].m_Cur.m_X, GameClient()->m_Snap.m_aCharacters[TargetClientId].m_Cur.m_Y);
	HammerTarget(Pos, OtherPos);
}

void CControls::AutoHammerNearby()
{
	if(!g_Config.m_TcAutoHammerNearby || GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter)
		return;

	const vec2 Pos = GameClient()->m_PredictedChar.m_Pos;
	int TargetClientId = -1;
	float ClosestDistance = 0.0f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!PiFuncCanAimClient(ClientId))
			continue;

		const vec2 OtherPos = vec2(GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_X, GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_Y);
		const float Dist = distance(Pos, OtherPos);
		if(Dist > CCharacterCore::PhysicalSize() * 1.75f)
			continue;

		if(TargetClientId == -1 || Dist < ClosestDistance)
		{
			TargetClientId = ClientId;
			ClosestDistance = Dist;
		}
	}

	if(TargetClientId == -1)
		return;

	const vec2 OtherPos = vec2(GameClient()->m_Snap.m_aCharacters[TargetClientId].m_Cur.m_X, GameClient()->m_Snap.m_aCharacters[TargetClientId].m_Cur.m_Y);
	HammerTarget(Pos, OtherPos);
}

void CControls::AutoHammerFrozenTeam()
{
	if(!g_Config.m_TcAutoHammerFrozenTeam || GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter)
		return;

	const vec2 Pos = GameClient()->m_PredictedChar.m_Pos;
	int TargetClientId = -1;
	float ClosestDistance = 0.0f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!PiFuncCanAimClient(ClientId) || !IsClientFrozen(ClientId))
			continue;

		const vec2 OtherPos = vec2(GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_X, GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_Y);
		const float Dist = distance(Pos, OtherPos);
		if(Dist > 76.0f)
			continue;

		if(TargetClientId == -1 || Dist < ClosestDistance)
		{
			TargetClientId = ClientId;
			ClosestDistance = Dist;
		}
	}

	if(TargetClientId == -1)
		return;

	const vec2 OtherPos = vec2(GameClient()->m_Snap.m_aCharacters[TargetClientId].m_Cur.m_X, GameClient()->m_Snap.m_aCharacters[TargetClientId].m_Cur.m_Y);
	HammerTarget(Pos, OtherPos);
}

void CControls::GunAimAssist()
{
	if(!g_Config.m_TcGunAimAssist || GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter)
		return;
	const int Weapon = GameClient()->m_PredictedChar.m_ActiveWeapon;
	if(Weapon != WEAPON_SHOTGUN && Weapon != WEAPON_LASER)
		return;

	const vec2 Pos = GameClient()->m_PredictedChar.m_Pos;
	vec2 Target = vec2(m_aInputData[g_Config.m_ClDummy].m_TargetX, m_aInputData[g_Config.m_ClDummy].m_TargetY);
	if(Target == vec2(0.0f, 0.0f))
		return;
	Target = normalize(Target);

	int BestClientId = -1;
	float BestAngle = 0.0f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!PiFuncCanAimClient(ClientId))
			continue;

		const CNetObj_Character &Char = GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur;
		const vec2 OtherPos = vec2(Char.m_X, Char.m_Y);
		const vec2 OtherVel = vec2(Char.m_VelX / 256.0f, Char.m_VelY / 256.0f);
		const float Lead = Weapon == WEAPON_LASER ? 1.0f : 1.8f;
		const vec2 Aim = OtherPos + OtherVel * Lead - Pos;
		const float Dist = length(Aim);
		if(Dist <= 0.0f || Dist > 850.0f)
			continue;

		vec2 CollisionPos;
		int TeleNr = 0;
		if(Collision()->IntersectLineTeleWeapon(Pos, OtherPos, &CollisionPos, nullptr, &TeleNr))
			continue;

		const float Angle = std::acos(std::clamp(dot(normalize(Aim), Target), -1.0f, 1.0f));
		if(Angle > g_Config.m_TcGunAimAssistAngle * pi / 180.0f)
			continue;

		if(BestClientId == -1 || Angle < BestAngle)
		{
			BestClientId = ClientId;
			BestAngle = Angle;
		}
	}

	if(BestClientId == -1)
		return;

	const CNetObj_Character &Char = GameClient()->m_Snap.m_aCharacters[BestClientId].m_Cur;
	const float Lead = Weapon == WEAPON_LASER ? 1.0f : 1.8f;
	const vec2 Aim = vec2(Char.m_X, Char.m_Y) + vec2(Char.m_VelX / 256.0f, Char.m_VelY / 256.0f) * Lead - Pos;
	m_aInputData[g_Config.m_ClDummy].m_TargetX = round_to_int(Aim.x);
	m_aInputData[g_Config.m_ClDummy].m_TargetY = round_to_int(Aim.y);
}

void CControls::FollowTee()
{
	if(!g_Config.m_TcFollowTee || GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter || m_aInputDirectionLeft[g_Config.m_ClDummy] || m_aInputDirectionRight[g_Config.m_ClDummy])
		return;
	if(Client()->GameTick(g_Config.m_ClDummy) <= m_AvoidFreezeMessageTick)
		return;

	const vec2 Pos = GameClient()->m_PredictedChar.m_Pos;
	int TargetClientId = -1;
	float ClosestDistance = 0.0f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!PiFuncCanAimClient(ClientId))
			continue;

		const vec2 OtherPos = vec2(GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_X, GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_Y);
		const float Dist = distance(Pos, OtherPos);
		if(Dist > 700.0f)
			continue;

		if(TargetClientId == -1 || Dist < ClosestDistance)
		{
			TargetClientId = ClientId;
			ClosestDistance = Dist;
		}
	}

	if(TargetClientId == -1)
		return;

	const CNetObj_Character &Target = GameClient()->m_Snap.m_aCharacters[TargetClientId].m_Cur;
	const vec2 OtherPos = vec2(Target.m_X, Target.m_Y);
	const vec2 OtherVel = vec2(Target.m_VelX / 256.0f, Target.m_VelY / 256.0f);
	const float DesiredDistance = 96.0f;
	const float DeltaX = OtherPos.x - Pos.x;
	if(absolute(DeltaX) > DesiredDistance)
		m_aInputData[g_Config.m_ClDummy].m_Direction = DeltaX > 0.0f ? 1 : -1;
	else if(absolute(OtherVel.x) > 5.0f)
		m_aInputData[g_Config.m_ClDummy].m_Direction = OtherVel.x > 0.0f ? 1 : -1;

	if(OtherPos.y + 48.0f < Pos.y || (OtherVel.y < -7.0f && distance(Pos, OtherPos) < 160.0f))
		m_aInputData[g_Config.m_ClDummy].m_Jump = 1;
}

void CControls::BalanceBot()
{
	if(!g_Config.m_TcBalanceBot || GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter || m_aInputDirectionLeft[g_Config.m_ClDummy] || m_aInputDirectionRight[g_Config.m_ClDummy])
		return;
	if(Client()->GameTick(g_Config.m_ClDummy) <= m_AvoidFreezeMessageTick)
		return;

	const vec2 Pos = GameClient()->m_PredictedChar.m_Pos;
	int TargetClientId = -1;
	float ClosestScore = 0.0f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!PiFuncCanAimClient(ClientId))
			continue;

		const CNetObj_Character &Target = GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur;
		const vec2 OtherPos = vec2(Target.m_X, Target.m_Y);
		const float Dx = OtherPos.x - Pos.x;
		const float Dy = OtherPos.y - Pos.y;
		if(absolute(Dx) > 240.0f || Dy < 18.0f || Dy > 150.0f)
			continue;

		const float Score = absolute(Dx) + absolute(Dy - 52.0f) * 0.35f;
		if(TargetClientId == -1 || Score < ClosestScore)
		{
			TargetClientId = ClientId;
			ClosestScore = Score;
		}
	}

	if(TargetClientId == -1)
		return;

	const CNetObj_Character &Target = GameClient()->m_Snap.m_aCharacters[TargetClientId].m_Cur;
	const vec2 OtherPos = vec2(Target.m_X, Target.m_Y);
	const vec2 OtherVel = vec2(Target.m_VelX / 256.0f, Target.m_VelY / 256.0f);
	const float PredictedCenterX = OtherPos.x + OtherVel.x * 3.0f;
	const float ErrorX = PredictedCenterX - Pos.x;

	if(absolute(ErrorX) > 10.0f)
		m_aInputData[g_Config.m_ClDummy].m_Direction = ErrorX > 0.0f ? 1 : -1;
	else if(absolute(OtherVel.x) > 4.0f)
		m_aInputData[g_Config.m_ClDummy].m_Direction = OtherVel.x > 0.0f ? 1 : -1;

	if(OtherPos.y - Pos.y < 42.0f && GameClient()->m_PredictedChar.m_Vel.y > 0.0f)
		m_aInputData[g_Config.m_ClDummy].m_Jump = 1;
}

void CControls::AutoDummySave()
{
	const auto ResetAutoDummySave = [&]() {
		if(m_AutoDummySaveActive)
		{
			g_Config.m_ClDummyHook = 0;
			m_AutoDummySaveActive = false;
		}
	};

	if(!g_Config.m_TcAutoDummySave || !Client()->DummyConnected() || !g_Config.m_ClDummyControl || GameClient()->m_aLocalIds[0] < 0 || GameClient()->m_aLocalIds[1] < 0)
	{
		ResetAutoDummySave();
		return;
	}

	const int MainId = GameClient()->m_aLocalIds[0];
	const int DummyId = GameClient()->m_aLocalIds[1];
	if(!GameClient()->m_Snap.m_aCharacters[MainId].m_Active || !GameClient()->m_Snap.m_aCharacters[DummyId].m_Active)
	{
		ResetAutoDummySave();
		return;
	}

	const CNetObj_Character &MainChar = GameClient()->m_Snap.m_aCharacters[MainId].m_Cur;
	const CNetObj_Character &DummyChar = GameClient()->m_Snap.m_aCharacters[DummyId].m_Cur;
	const vec2 MainPos = vec2(MainChar.m_X, MainChar.m_Y);
	const vec2 DummyPos = vec2(DummyChar.m_X, DummyChar.m_Y);
	const vec2 MainVel = vec2(MainChar.m_VelX / 256.0f, MainChar.m_VelY / 256.0f);

	const bool MainFalling = MainVel.y > 8.0f || MainPos.y > DummyPos.y + 220.0f;
	if(!MainFalling || distance(MainPos, DummyPos) > (float)GameClient()->m_aTuning[1].m_HookLength)
	{
		ResetAutoDummySave();
		return;
	}

	vec2 CollisionPos;
	int TeleNr = 0;
	const vec2 Aim = MainPos + MainVel * 3.0f - DummyPos;
	if(length(Aim) <= 0.0f)
	{
		ResetAutoDummySave();
		return;
	}
	const vec2 HookStart = DummyPos + normalize(Aim) * CCharacterCore::PhysicalSize() * 1.5f;
	if(Collision()->IntersectLineTeleHook(HookStart, MainPos, &CollisionPos, nullptr, &TeleNr))
	{
		ResetAutoDummySave();
		return;
	}

	g_Config.m_ClDummyHook = 1;
	g_Config.m_ClDummyJump = 0;
	g_Config.m_ClDummyFire = 0;
	m_AutoDummySaveActive = true;
	GameClient()->m_DummyInput.m_TargetX = round_to_int(Aim.x);
	GameClient()->m_DummyInput.m_TargetY = round_to_int(Aim.y);
	if(absolute(Aim.x) > 96.0f)
		GameClient()->m_DummyInput.m_Direction = Aim.x > 0.0f ? 1 : -1;
}

int CControls::SnapInput(int *pData)
{
	// update player state
	if(GameClient()->m_Chat.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(GameClient()->m_Menus.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(GameClient()->m_Scoreboard.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	switch(m_aMouseInputType[g_Config.m_ClDummy])
	{
	case CControls::EMouseInputType::AUTOMATED:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
		break;
	case CControls::EMouseInputType::ABSOLUTE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
		break;
	case CControls::EMouseInputType::RELATIVE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
		break;
	}

	// TClient
	if(g_Config.m_TcHideChatBubbles && Client()->RconAuthed())
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags &= ~PLAYERFLAG_CHATTING;

	if(g_Config.m_TcNameplatePingCircle)
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	// we freeze the input if chat or menu is activated
	if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
			ResetInput(g_Config.m_ClDummy);

		mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		vec2 Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// send once a second just to be sure
		Send = Send || time_get() > m_LastSendTime + time_freq();
	}
	else
	{
		// TClient
		vec2 Pos;
		if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
		{
			Pos = GameClient()->m_Controls.m_aMousePosOnAction[g_Config.m_ClDummy];
			m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
		}
		else
			Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];

		m_FastInputHookAction = false;
		m_FastInputFireAction = false;

		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		LogAimCorrectionResult();
		GunAimAssist();
		ForgiveHook();
		EmoteSpammer();
		AutoLed();
		AutoHammerNearby();
		AutoHammerFrozenTeam();
		AutoDummySave();

		// set direction
		m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
		if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

		AvoidFreeze();
		FollowTee();
		BalanceBot();

		// dummy copy moves
		if(g_Config.m_ClDummyCopyMoves)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;

			// Don't copy any input to dummy when spectating others
			if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
			{
				pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
				pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
				pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
				pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
				pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
				pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
				pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

				if(!g_Config.m_ClDummyControl)
					pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

				pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
				pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
			}

			m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
		}

		// stress testing
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

			m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
			m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
			m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
			m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}

		// check if we need to send input
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
	}

	CNetObj_PlayerInput SendInput = m_aInputData[g_Config.m_ClDummy];
	if(m_AvoidFreezeJumped)
		m_aInputData[g_Config.m_ClDummy].m_Jump = 0;
	if(m_AvoidFreezeHooked && Client()->GameTick(g_Config.m_ClDummy) >= m_AvoidFreezeHookUntilTick)
	{
		m_aInputData[g_Config.m_ClDummy].m_Hook = 0;
		m_AvoidFreezeHooked = false;
		m_AvoidFreezeHookCooldownTick = Client()->GameTick(g_Config.m_ClDummy) + 6;
	}

	// copy and return size
	m_aLastData[g_Config.m_ClDummy] = SendInput;

	if(!Send)
		return 0;

	m_LastSendTime = time_get();
	mem_copy(pData, &SendInput, sizeof(m_aInputData[0]));
	return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		m_aAmmoCount[maximum(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
		float Zoom = GameClient()->m_Camera.m_Zoom;
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
	{
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
	}
	else
	{
		m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
	}

}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(GameClient()->m_Snap.m_pGameInfoObj && (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return false;

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
		{
			m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
			GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::ABSOLUTE;
		}
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
		}
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= GameClient()->m_Camera.m_Zoom;

	m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
	GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::RELATIVE;
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
	{
		const float MouseMin = GetMinMouseDistance();
		const float MouseMax = GetMaxMouseDistance();

		float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance < 0.001f)
		{
			m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
			m_aMousePos[g_Config.m_ClDummy].y = 0;
			MouseDistance = 0.001f;
		}
		if(MouseDistance < MouseMin)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
		MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance > MouseMax)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;

		if(g_Config.m_TcLimitMouseToScreen)
		{
			float Width, Height;
			Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), 1.0f, &Width, &Height);
			Height /= 2.0f;
			Width /= 2.0f;
			if(g_Config.m_TcLimitMouseToScreen == 2)
				Width = Height;
			m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -Height, Height);
			m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -Width, Width);
		}
	}
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return minimum((FollowFactor != 0 ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
}

bool CControls::CheckNewInput()
{
	bool NewInput[2] = {};
	for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
	{
		CNetObj_PlayerInput TestInput = m_aInputData[Dummy];
		if(Dummy == g_Config.m_ClDummy)
		{
			TestInput.m_Direction = 0;
			if(m_aInputDirectionLeft[Dummy] && !m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = -1;
			if(!m_aInputDirectionLeft[Dummy] && m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = 1;
		}

		if(m_aFastInput[Dummy].m_Direction != TestInput.m_Direction)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Hook != TestInput.m_Hook)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Jump != TestInput.m_Jump)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_NextWeapon != TestInput.m_NextWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_PrevWeapon != TestInput.m_PrevWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_WantedWeapon != TestInput.m_WantedWeapon)
			NewInput[Dummy] = true;

		bool SetMousePos = false;
		// We need to be careful about how we manage the mouse position to avoid mispredicted hooks and fires
		// on the first tick that they activate before we know what mouse position we actually sent to the server
		if(Dummy == g_Config.m_ClDummy)
		{
			if(m_aFastInput[Dummy].m_Hook == 0 && TestInput.m_Hook == 1)
			{
				m_FastInputHookAction = true;
				SetMousePos = true;
			}
			if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire && TestInput.m_Fire % 2 == 1)
			{
				m_FastInputFireAction = true;
				SetMousePos = true;
			}
			if(!m_FastInputHookAction && !m_FastInputFireAction)
			{
				SetMousePos = true;
			}
		}

		if(SetMousePos)
		{
			TestInput.m_TargetX = (int)m_aMousePos[Dummy].x;
			TestInput.m_TargetY = (int)m_aMousePos[Dummy].y;
		}
		else
		{
			TestInput.m_TargetX = m_aFastInput[Dummy].m_TargetX;
			TestInput.m_TargetY = m_aFastInput[Dummy].m_TargetY;
		}

		m_aFastInput[Dummy] = TestInput;
	}

	if(NewInput[0] || NewInput[1])
		return true;
	else
		return false;
}
