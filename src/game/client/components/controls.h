/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CONTROLS_H
#define GAME_CLIENT_COMPONENTS_CONTROLS_H

#include <base/vmath.h>

#include <engine/client.h>
#include <engine/console.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls : public CComponent
{
public:
	float GetMinMouseDistance() const;
	float GetMaxMouseDistance() const;

	enum class EMouseInputType
	{
		ABSOLUTE,
		RELATIVE,
		AUTOMATED,
	};

	vec2 m_aMousePos[NUM_DUMMIES];
	vec2 m_aMousePosOnAction[NUM_DUMMIES];
	vec2 m_aTargetPos[NUM_DUMMIES];

	EMouseInputType m_aMouseInputType[NUM_DUMMIES];

	int m_aAmmoCount[NUM_WEAPONS];

	int64_t m_LastSendTime;
	CNetObj_PlayerInput m_aInputData[NUM_DUMMIES];
	CNetObj_PlayerInput m_aLastData[NUM_DUMMIES];
	int m_aInputDirectionLeft[NUM_DUMMIES];
	int m_aInputDirectionRight[NUM_DUMMIES];
	int m_aShowHookColl[NUM_DUMMIES];

	// TClient
	CNetObj_PlayerInput m_aFastInput[NUM_DUMMIES];
	bool m_FastInputHookAction = false;
	bool m_FastInputFireAction = false;
	int m_AvoidFreezeMessageTick = 0;
	bool m_AvoidFreezeJumped = false;
	bool m_AutoDummySaveActive = false;
	int64_t m_LastEmoteSpamTime = 0;
	int m_EmoteSpamIndex = 0;
	bool m_AimCorrectionLogHeaderWritten = false;
	bool m_AimCorrectionResultLogHeaderWritten = false;
	int m_AimCorrectionPendingTick = -1;
	int m_AimCorrectionPendingClientId = -1;
	float m_AimCorrectionPendingLateralMiss = 0.0f;
	int AvoidFreezeMessageTick() const { return m_AvoidFreezeMessageTick; }

	CControls();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	void OnConsoleInit() override;
	virtual void OnPlayerDeath();

	int SnapInput(int *pData);
	void ClampMousePos();
	void ResetInput(int Dummy);
	bool CheckNewInput();
	void AvoidFreeze();
	void ForgiveHook();
	void AutoLed();
	void AutoHammerNearby();
	void AutoHammerFrozenTeam();
	void GunAimAssist();
	void FollowTee();
	void BalanceBot();
	void AutoDummySave();
	void EmoteSpammer();

private:
	bool IsFreezeTile(vec2 Pos);
	bool IsSingleFreezeTile(vec2 Pos);
	bool IsClientFrozen(int ClientId) const;
	void HammerTarget(vec2 Pos, vec2 TargetPos);
	bool PiFuncCanAimClient(int ClientId) const;
	void LogAimCorrection(int ClientId, vec2 Pos, vec2 TargetPos, vec2 TargetVel, vec2 OldAim, vec2 NewAim, float TravelTicks, float LateralMiss);
	void LogAimCorrectionResult();
	static void ConKeyInputState(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputSet(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData);
};
#endif
