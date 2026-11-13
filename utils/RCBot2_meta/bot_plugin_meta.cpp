// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
/**
 * vim: set ts=4 sw=4 tw=99 noet :
 * ======================================================
 * Metamod:Source Sample Plugin
 * Written by AlliedModders LLC.
 * ======================================================
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from 
 * the use of this software.
 *
 * This sample plugin is public domain.
 */

#include <cstdio>

#pragma push_macro("clamp") //Fix for C++17 [APG]RoboCop[CL]
#undef clamp
#include <algorithm>
#pragma pop_macro("clamp")

#include "bot_plugin_meta.h"

#include "filesystem.h"
#include "interface.h"
#include "IStaticPropMgr.h"

#ifdef __linux__
#include "shake.h"    //bir3yk
#endif

#include "ndebugoverlay.h"
#include "irecipientfilter.h"

#include "bot_cvars.h"

#if defined(_WIN64) || defined(_WIN32)
	#include <ctime>
#endif

// for IServerTools
#include "bot.h"
#include "bot_configfile.h"
#include "bot_globals.h"
#include "bot_profile.h"
#include "bot_menu.h"
#include "bot_getprop.h"
#include "bot_event.h"
#include "bot_profiling.h"
#include "bot_wpt_dist.h"
#include "bot_squads.h"
#include "bot_accessclient.h"
#include "bot_weapons.h"
#include "bot_waypoint_visibility.h"
#include "bot_kv.h"
#include "bot_sigscan.h"
#include "bot_mods.h"

#include "tier0/icommandline.h"

#include "rcbot/logging.h"

#include <cstring>

#include <build_info.h>
#include <sourcehook.h>

#ifdef SM_EXT
#include "rcbot/entprops.h"

#ifdef RCBOT_VPROF_ENABLED
#include <tier0/vprof.h>
#endif // RCBOT_VPROF_ENABLED
#endif

// SourceHook SH_DECL_HOOK* macros use implicit false-to-int casts and uninitialized orig_ret for void hooks - [APG]RoboCop[CL]
SH_DECL_HOOK6(IServerGameDLL, LevelInit, SH_NOATTRIB, 0, bool, char const *, char const *, char const *, char const *, bool, bool);
SH_DECL_HOOK3_void(IServerGameDLL, ServerActivate, SH_NOATTRIB, 0, edict_t *, int, int);
SH_DECL_HOOK1_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool);
SH_DECL_HOOK0_void(IServerGameDLL, LevelShutdown, SH_NOATTRIB, 0);
SH_DECL_HOOK2_void(IServerGameClients, ClientActive, SH_NOATTRIB, 0, edict_t *, bool);
SH_DECL_HOOK1_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, edict_t *);
SH_DECL_HOOK2_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, edict_t *, char const *);
SH_DECL_HOOK1_void(IServerGameClients, SetCommandClient, SH_NOATTRIB, 0, int);
SH_DECL_HOOK5(IServerGameClients, ClientConnect, SH_NOATTRIB, 0, bool, edict_t *, const char*, const char *, char *, int);
SH_DECL_HOOK2(IGameEventManager2, FireEvent, SH_NOATTRIB, 0, bool, IGameEvent *, bool);
SH_DECL_HOOK2(IVEngineServer, UserMessageBegin, SH_NOATTRIB, 0, bf_write *, IRecipientFilter *, int);
SH_DECL_HOOK0_void(IVEngineServer, MessageEnd, SH_NOATTRIB, 0);

#if SOURCE_ENGINE >= SE_ORANGEBOX
SH_DECL_HOOK2_void(IServerGameClients, NetworkIDValidated, SH_NOATTRIB, 0, const char *, const char *);
SH_DECL_HOOK2_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, edict_t *, const CCommand &);
#else
SH_DECL_HOOK1_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, edict_t *);
#endif

SH_DECL_MANUALHOOK2_void(MHook_PlayerRunCmd, 0, 0, 0, CUserCmd*, IMoveHelper*);
// [crashfix] DOD:S rocket owner crash guard (server.dll + 0x2a2ad4).
//
// The crashing write happens in CDODBaseRocketWeapon::vfunc0x3fc, which is
// dispatched from several CDODPlayer virtuals that all follow the same shape:
//
//   activeWeapon = GetActiveWeapon();       // player + 0x81c
//   if (activeWeapon) activeWeapon->vfunc0x3fc(0);
//
// The two CDODPlayer slots that are reached by the death path are
//   slot 376 (0x5e0 -> 0x101e36d0)  <- confirmed crash path (crash dump of
//                                         the 2026-11-12 repro: server.dll
//                                         +0x1e3776 -> +0x2a2ad4)
//   slot 348 (0x570 -> 0x101e1380)  <- reached from CDODPlayer::vfunc0x56c
//
// Both take a single int argument, so both are hooked with the same handler.
// The slots come from hookinfo.ini / rcbot_weapon_guard_dods[_b] and are
// RECONFIGUREd in Load().
SH_DECL_MANUALHOOK1_void(MHook_DodWeaponGuard, 0, 0, 0, int);
SH_DECL_MANUALHOOK1_void(MHook_DodWeaponGuardB, 0, 0, 0, int);

// set in Load() once hookinfo + netprop offsets are known; gates ADD/REMOVE of
// the guard hook in Hook_ClientPutInServer / Hook_ClientDisconnect
static bool s_bDodWeaponGuardEnabled = false;
static bool s_bDodWeaponGuardEnabledB = false;

// [crashfix] A/B instrumentation. The A/B logs proved "guard OFF crashes,
// guard ON does not", but the handler counters showed calls=1 over 380 repro
// kills, so "no crash" could not be attributed to the guard. These allow the
// next run to distinguish "hook not on the instance" (H1) from "crash chain
// entry never ran" (H2):
//  - s_pOrigVtbl34x/37x: the vtable entries BEFORE the first guard hook was
//    added (captured in Hook_ClientPutInServer). Any instance whose current
//    entry differs from these has the SourceHook stub installed, i.e. a
//    dispatch through that slot MUST reach Hook_DodWeaponGuard.
//  - s_uGuardCalls/s_uGuardCleared: the handler's counters, moved to file
//    scope so the GameFrame heartbeat can print them next to the install
//    state.
//  - the heartbeat (every 10s while the guard is on) prints one line with
//    client count, hooked count per slot and the handler counters.
static void *s_pOrigVtbl347 = nullptr;
static void *s_pOrigVtbl348 = nullptr;
static void *s_pOrigVtbl376 = nullptr;
static bool s_bOrigVtblCaptured = false;
static unsigned s_uGuardCalls = 0;
static unsigned s_uGuardCleared = 0;
static unsigned s_uGuardRetActiveInvalid = 0;
static unsigned s_uGuardRetOwnerIsPlayer = 0;
static unsigned s_uGuardRetNoProp = 0;
static float s_flNextInstallProbe = 0.0f;

// rcbot_loglevel is defined in rcbot/logging.cpp but not declared in
// logging.h; the probe prints it so a silent logger can be ruled out as
// a level filter
extern ConVar rcbot_loglevel;

IServerGameDLL *server = nullptr;
IGameEventManager2 *gameevents = nullptr;
IServerPluginCallbacks *vsp_callbacks = nullptr;
ICvar *icvar = nullptr;
IVEngineServer *engine = nullptr;  // helper functions (messaging clients, loading content, making entities, running commands, etc)
IFileSystem *filesystem = nullptr;  // file I/O 
IGameEventManager2 *gameeventmanager = nullptr;
IGameEventManager *gameeventmanager1 = nullptr;  // game events interface
IPlayerInfoManager *playerinfomanager = nullptr;  // game dll interface to interact with players
IServerPluginHelpers *helpers = nullptr;  // special 3rd party plugin helpers from the engine
IServerGameClients* gameclients = nullptr;
IEngineTrace *enginetrace = nullptr;
IEffects *g_pEffects = nullptr;
IBotManager *g_pBotManager = nullptr;
CGlobalVars *gpGlobals = nullptr;
IVDebugOverlay *debugoverlay = nullptr;
IServerGameEnts *servergameents = nullptr; // for accessing the server game entities
IServerGameDLL *servergamedll = nullptr;
IServerTools *servertools = nullptr;
IStaticPropMgrServer* staticpropmgr = nullptr;

RCBotPluginMeta g_RCBotPluginMeta;

PLUGIN_EXPOSE(RCBotPluginMeta, g_RCBotPluginMeta);

static ConVar rcbot2_ver_cvar("rcbot_ver", build_info::long_version, FCVAR_REPLICATED, "RCBot version");

CON_COMMAND(rcbotd, "access the bot commands on a server")
{
	if (!engine->IsDedicatedServer() || !CBotGlobals::IsMapRunning())
	{
		logger->Log(LogLevel::ERROR, "Error, no map running or not dedicated server");
		return;
	}

	// shift args and call subcommand
	BotCommandArgs argList;
	for (unsigned i = 1; i <= static_cast<unsigned>(args.ArgC()); i++) {
		argList.emplace_back(args.Arg(static_cast<int>(i)));
	}
	const eBotCommandResult iResult = CBotGlobals::m_pCommands->execute(nullptr, argList);

	if (iResult == COMMAND_ACCESSED)
	{
		// ok
	}
	else if (iResult == COMMAND_REQUIRE_ACCESS)
	{
		logger->Log(LogLevel::ERROR, "You do not have access to this command");
	}
	else if (iResult == COMMAND_NOT_FOUND)
	{
		logger->Log(LogLevel::ERROR, "bot command not found");
	}
	else if (iResult == COMMAND_ERROR)
	{
		logger->Log(LogLevel::ERROR, "bot command returned an error");
	}
}

class CBotRecipientFilter : public IRecipientFilter
{
public:
	CBotRecipientFilter(const edict_t *pPlayer)
	{
		m_iPlayerSlot = ENTINDEX(pPlayer);
	}

	bool IsReliable() const override { return false; }
	bool IsInitMessage() const override { return false; }

	int	GetRecipientCount() const override { return 1; }
	int	GetRecipientIndex(int slot) const override { return m_iPlayerSlot; }

private:
	int m_iPlayerSlot;
};

class CClientBroadcastRecipientFilter : public IRecipientFilter
{
public:

	CClientBroadcastRecipientFilter() : m_iMaxCount(0) {

		// Initialize m_iPlayerSlot with a default value, e.g., -1
		std::fill(std::begin(m_iPlayerSlot), std::end(m_iPlayerSlot), -1);

		for (int i = 0; i < RCBOT_MAXPLAYERS; ++i) {
			const CClient* client = CClients::get(i);

			if (client->isUsed()) {
				IPlayerInfo* p = playerinfomanager->GetPlayerInfo(client->getPlayer());

				if (p->IsConnected() && !p->IsFakeClient()) {
					m_iPlayerSlot[m_iMaxCount] = i;
					m_iMaxCount++;
				}
			}
		}
	}

	bool IsReliable() const override { return false; }
	bool IsInitMessage() const override { return false; }

	int	GetRecipientCount() const override { return m_iMaxCount; }
	int	GetRecipientIndex(const int slot) const override { return m_iPlayerSlot[slot] + 1; }

private:

	int m_iMaxCount;
	int m_iPlayerSlot[RCBOT_MAXPLAYERS];
};

///////////////
// hud message
///////////////
void RCBotPluginMeta::HudTextMessage(const edict_t* pEntity, const char* szMessage)
{
	int msgid = 0;
	int imsgsize = 0;
	char msgbuf[64];
	bool bOK; //Unused? [APG]RoboCop[CL]

	int hint = -1;
	int say = -1;

	while ((bOK = servergamedll->GetUserMessageInfo(msgid, msgbuf, 63, imsgsize)) == true)
	{
		if (std::strcmp(msgbuf, "HintText") == 0)
			hint = msgid;
		else if (std::strcmp(msgbuf, "SayText") == 0)
			say = msgid;

		msgid++;
	}

	if (msgid == 0)
		return;

	// if (!bOK)
	// return;

	CBotRecipientFilter filter(pEntity);

	bf_write* buf;

	if (hint > 0) {
		buf = engine->UserMessageBegin(&filter, hint);
		buf->WriteString(szMessage);
		engine->MessageEnd();
	}

	if (say > 0) {
		char chatline[128];
		snprintf(chatline, sizeof(chatline), "\x01\x04[RCBot2]\x01 %s\n", szMessage);

		buf = engine->UserMessageBegin(&filter, say);
		buf->WriteString(chatline);
		engine->MessageEnd();
	}
}

//////////////////////////
// chat broadcast message
//////////////////////////
void RCBotPluginMeta::BroadcastTextMessage(const char* szMessage)
{
	int msgid = 0;
	int imsgsize = 0;
	char msgbuf[64];
	bool bOK; //Unused? [APG]RoboCop[CL]

	int hint = -1; //Unused? [APG]RoboCop[CL]
	int say = -1;

	while ((bOK = servergamedll->GetUserMessageInfo(msgid, msgbuf, 63, imsgsize)) == true)
	{
		if (std::strcmp(msgbuf, "HintText") == 0)
			hint = msgid;
		else if (std::strcmp(msgbuf, "SayText") == 0)
			say = msgid;

		msgid++;
	}

	if (msgid == 0)
		return;

	CClientBroadcastRecipientFilter filter; // Allocate on stack
	if (say > 0) {
		char chatline[128];
		snprintf(chatline, sizeof(chatline), "\x01\x04[RCBot2]\x01 %s\n", szMessage);

		bf_write* buf = engine->UserMessageBegin(&filter, say);
		buf->WriteString(chatline);
		engine->MessageEnd();
	}
}

bf_write *RCBotPluginMeta::Hook_UserMessageBegin(IRecipientFilter *pFilter, int iMsgType)
{
	// Resolve the message index once (-2 = looked up and not present, so we stop searching).
	if (m_iSetPlayerLocationMsg == -1)
	{
		m_iSetPlayerLocationMsg = -2;

		int iId = 0;
		char szName[64];
		int iSize = 0;

		while (servergamedll->GetUserMessageInfo(iId, szName, sizeof(szName) - 1, iSize))
		{
			if (std::strcmp(szName, "SetPlayerLocation") == 0)
			{
				m_iSetPlayerLocationMsg = iId;
				break;
			}

			iId++;
		}
	}

	if (iMsgType == m_iSetPlayerLocationMsg && pFilter != nullptr && pFilter->GetRecipientCount() > 0)
	{
		m_iPendingLocationRecipient = pFilter->GetRecipientIndex(0); // player entindex
		m_pPendingLocationBuf = META_RESULT_ORIG_RET(bf_write *);    // buffer FF writes the name into
	}

	RETURN_META_VALUE(MRES_IGNORED, nullptr);
}

void RCBotPluginMeta::Hook_MessageEnd()
{
	if (m_iPendingLocationRecipient > 0 && m_pPendingLocationBuf != nullptr)
	{
		bf_read read(m_pPendingLocationBuf->GetData(), m_pPendingLocationBuf->GetNumBytesWritten());

		char szLocation[128];
		read.ReadString(szLocation, sizeof(szLocation));

		const int iSlot = m_iPendingLocationRecipient - 1; // entindex -> 0-based client slot

		if (iSlot >= 0 && iSlot < RCBOT_MAXPLAYERS)
			CClients::get(iSlot)->setLocation(szLocation);
	}

	m_iPendingLocationRecipient = -1;
	m_pPendingLocationBuf = nullptr;

	RETURN_META(MRES_IGNORED);
}

void RCBotPluginMeta::Hook_PlayerRunCmd(CUserCmd *ucmd, IMoveHelper *moveHelper)
{
#ifdef RCBOT_VPROF_ENABLED
	VPROF_BUDGET("RCBotPluginMeta::Hook_PlayerRunCmd", "RCBot2")
#endif // RCBOT_VPROF_ENABLED

	// [FIX] Crash3: this vtable hook can fire for a player whose entity is being
	// torn down (disconnect/level change) before Hook_ClientDisconnect removes the
	// hook, and ucmd/moveHelper are not guaranteed non-null by the engine on every
	// call path. Previously servergameents/pPlayer/ucmd were dereferenced with no
	// checks -> null/use-after-free crash. Also drop the function-local statics so
	// a previous bot's pointer can never be mistaken for the current one.
	if ( servergameents == nullptr )
		RETURN_META(MRES_IGNORED);

	CBaseEntity *pPlayer = META_IFACEPTR(CBaseEntity);

	if ( pPlayer == nullptr )
		RETURN_META(MRES_IGNORED);

	const edict_t *pEdict = servergameents->BaseEntityToEdict(pPlayer);

	CBot *pBot = CBots::getBotPointer(pEdict);

	if ( pBot && ucmd )
	{
		CBotCmd *cmd = pBot->getUserCMD();

		if ( cmd != nullptr )
		{
			// put the bot's commands into this move frame
			ucmd->buttons = cmd->buttons;
			ucmd->forwardmove = cmd->forwardmove;
			ucmd->impulse = cmd->impulse;
			ucmd->sidemove = cmd->sidemove;
			ucmd->upmove = cmd->upmove;
			ucmd->viewangles = cmd->viewangles;
			ucmd->weaponselect = cmd->weaponselect;
			ucmd->weaponsubtype = cmd->weaponsubtype;
			ucmd->tick_count = cmd->tick_count;
			ucmd->command_number = cmd->command_number;
		}
	}
	RETURN_META(MRES_IGNORED);
}

// [crashfix] DoD:S rocket owner crash -- server.dll + 0x2a2ad4
// (Accelerator IDs M4ER-I5AC-TEM6).
//
// Crash stack (verified with the minidump of the 2026-11-12 repro):
//   RCBot2 -> CDODPlayer::vfunc0x428 (PlayerRunCommand, slot 425)
//     -> ... -> CDODPlayer::vfunc0x5e0 (0x101e36d0, slot 376)
//     -> weapon vfunc 0x3fc (0x102a2ab0, CWeaponBazooka/CWeaponPschreck)
//         owner = resolve(weapon->m_hOwner)   // weapon+0x49c, may be null
//         if (owner && owner->IsPlayer()) ok else owner = 0
//         mov byte [owner + 0x1428], 0        // <-- unconditional: crash
//
// On death the game clears the weapon owner while m_hActiveWeapon still points
// at the rocket launcher, so the write goes through a null pointer (MSVC UB
// turned into a guaranteed crash). server.dll cannot be patched from here, so
// before the original vfunc runs we validate the active weapon's m_hOwner
// ourselves and, if it does not resolve to a live player, invalidate
// m_hActiveWeapon. The original re-reads m_hActiveWeapon (0x100e07e0) and
// takes its own null branch, so the weapon virtual is never dispatched.
//
// The same handler is installed on two CDODPlayer slots (376 = the confirmed
// crash path, 348 = the other death path dispatch of the active weapon), so
// whichever of them runs first neutralises the bad state for all the others.
//
// The counters below exist so an A/B run can tell "the guard fired and
// neutralised the bad state" apart from "the repro never reached the hook":
// without them a run with no crash proves nothing on its own.
void RCBotPluginMeta::Hook_DodWeaponGuard(int arg)
{
	// `arg` is the int the game passes to the hooked vfunc (1 or 2 from the
	// death path, see the disassembly above)
	(void)arg;

	static float s_flNextLog = 0.0f;
	static float s_flNextProbe = 0.0f;

	// counters live at file scope so the GameFrame heartbeat can report them
	// next to the per-instance install state (see s_uGuardCalls)
	unsigned &s_uCalls = s_uGuardCalls;
	unsigned &s_uCleared = s_uGuardCleared;
	unsigned &s_uRetActiveInvalid = s_uGuardRetActiveInvalid;
	unsigned &s_uRetOwnerIsPlayer = s_uGuardRetOwnerIsPlayer;
	unsigned &s_uRetNoProp = s_uGuardRetNoProp;

	// logged at entry, before any early return, so a hook that only ever
	// bails out still reports that it ran
	++s_uCalls;

	if ( gpGlobals != nullptr && gpGlobals->curtime >= s_flNextProbe )
	{
		s_flNextProbe = gpGlobals->curtime + 5.0f;
		// META_LOG instead of logger->Log: on the 2026-11-13 A/B run the
		// logger's Msg() output vanished from stdout after ~90s while
		// META_LOG kept flowing for the whole session, which made the
		// handler counters unreadable. META_LOG also lands in the engine
		// log file, so the data survives either way.
		META_LOG(g_PLAPI,
			"[crashfix] guard probe: calls=%u cleared=%u ret_active_invalid=%u ret_owner_is_player=%u ret_no_prop=%u loglevel=%d",
			s_uCalls, s_uCleared, s_uRetActiveInvalid, s_uRetOwnerIsPlayer, s_uRetNoProp,
			rcbot_loglevel.GetInt());
	}

	if ( servergameents == nullptr || gpGlobals == nullptr || engine == nullptr )
		RETURN_META(MRES_IGNORED);

	CBaseEntity *pPlayer = META_IFACEPTR(CBaseEntity);

	if ( pPlayer == nullptr )
		RETURN_META(MRES_IGNORED);

	edict_t *pEdict = servergameents->BaseEntityToEdict(pPlayer);

	if ( pEdict == nullptr )
		RETURN_META(MRES_IGNORED);

	CBaseHandle *pActive = CClassInterface::getActiveWeaponHandle(pEdict);

	// netprop never resolved: never clear a weapon on a guess
	if ( pActive == nullptr )
	{
		++s_uRetNoProp;
		RETURN_META(MRES_IGNORED);
	}

	// no active weapon -> the original resolves null by itself
	if ( !pActive->IsValid() )
	{
		++s_uRetActiveInvalid;
		RETURN_META(MRES_IGNORED);
	}

	const int iWeapon = pActive->GetEntryIndex();

	// bound check before touching the entity list (invalid handles resolve to
	// NUM_ENT_ENTRIES-1, which is past maxEntities)
	if ( iWeapon < 1 || iWeapon >= gpGlobals->maxEntities )
		RETURN_META(MRES_IGNORED);

	edict_t *pWeaponEdict = engine->PEntityOfEntIndex(iWeapon);

	if ( pWeaponEdict == nullptr || pWeaponEdict->GetUnknown() == nullptr ||
		pWeaponEdict->GetUnknown()->GetBaseEntity() == nullptr )
		RETURN_META(MRES_IGNORED);

	// stale serial -> original resolves null as well
	if ( pWeaponEdict->GetUnknown()->GetRefEHandle().ToInt() != pActive->ToInt() )
		RETURN_META(MRES_IGNORED);

	CBaseHandle *pOwner = CClassInterface::getWeaponOwnerHandle(pWeaponEdict);

	// netprop never resolved: cannot tell a bad owner from a missing offset
	if ( pOwner == nullptr )
	{
		++s_uRetNoProp;
		RETURN_META(MRES_IGNORED);
	}

	bool bOwnerIsPlayer = false;

	if ( pOwner->IsValid() )
	{
		const int iOwner = pOwner->GetEntryIndex();

		// player edicts live in 1..maxClients, which also excludes worldspawn
		// (0) and every non-player entity the game's own IsPlayer() test would
		// reject - so the write at owner+0x1428 always has a live target
		if ( iOwner >= 1 && iOwner <= gpGlobals->maxClients )
		{
			edict_t *pOwnerEdict = engine->PEntityOfEntIndex(iOwner);

			if ( pOwnerEdict != nullptr && pOwnerEdict->GetUnknown() != nullptr )
			{
				IServerUnknown *pOwnerUnknown = pOwnerEdict->GetUnknown();

				// serial number must match, otherwise the slot was reused
				bOwnerIsPlayer = ( pOwnerUnknown->GetBaseEntity() != nullptr &&
					pOwnerUnknown->GetRefEHandle().ToInt() == pOwner->ToInt() );
			}
		}
	}

	if ( bOwnerIsPlayer )
	{
		++s_uRetOwnerIsPlayer;
		RETURN_META(MRES_IGNORED);
	}

	const bool bCleared = CClassInterface::clearActiveWeapon(pEdict);

	if ( bCleared )
		++s_uCleared;

	if ( bCleared && gpGlobals->curtime >= s_flNextLog )
	{
		s_flNextLog = gpGlobals->curtime + 5.0f;
		// META_LOG: this is the line that proves the guard actually fired on
		// the crash path, so it must not depend on the logger channel that
		// went silent on the 2026-11-13 A/B run
		META_LOG(g_PLAPI,
			"[crashfix] client %d: active weapon owner invalid (owner idx %d, valid %d) -> cleared m_hActiveWeapon, avoided server.dll+0x2a2ad4",
			IndexOfEdict(pEdict),
			pOwner->IsValid() ? pOwner->GetEntryIndex() : -1,
			pOwner->IsValid() ? 1 : 0);
	}

	RETURN_META(MRES_IGNORED);
}

/** 
 * Something like this is needed to register cvars/CON_COMMANDs.
 */
class BaseAccessor : public IConCommandBaseAccessor
{
public:
	virtual ~BaseAccessor() = default;

	bool RegisterConCommandBase(ConCommandBase *pCommandBase) override
	{
		/* Always call META_REGCVAR instead of going through the engine. */
		return META_REGCVAR(pCommandBase);
	}
} s_BaseAccessor;

bool RCBotPluginMeta::Load(PluginId id, ISmmAPI *ismm, char *error, std::size_t maxlen, bool late)
{
	extern MTRand_int32 irand;

	PLUGIN_SAVEVARS()

	GET_V_IFACE_CURRENT(GetEngineFactory, enginetrace, IEngineTrace, INTERFACEVERSION_ENGINETRACE_SERVER)
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER)
	GET_V_IFACE_CURRENT(GetEngineFactory, gameevents, IGameEventManager2, INTERFACEVERSION_GAMEEVENTSMANAGER2)
	GET_V_IFACE_CURRENT(GetEngineFactory, helpers, IServerPluginHelpers, INTERFACEVERSION_ISERVERPLUGINHELPERS)
	GET_V_IFACE_CURRENT(GetEngineFactory, icvar, ICvar, CVAR_INTERFACE_VERSION)
	GET_V_IFACE_CURRENT(GetEngineFactory, staticpropmgr, IStaticPropMgrServer, INTERFACEVERSION_STATICPROPMGR_SERVER)

	GET_V_IFACE_ANY(GetEngineFactory, filesystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION)

	GET_V_IFACE_ANY(GetServerFactory, servergameents, IServerGameEnts, INTERFACEVERSION_SERVERGAMEENTS)
	GET_V_IFACE_ANY(GetServerFactory, server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL)
	GET_V_IFACE_ANY(GetServerFactory, gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS)
	GET_V_IFACE_ANY(GetServerFactory, playerinfomanager, IPlayerInfoManager, INTERFACEVERSION_PLAYERINFOMANAGER)

	GET_V_IFACE_ANY(GetServerFactory, g_pEffects, IEffects, IEFFECTS_INTERFACE_VERSION)
	GET_V_IFACE_ANY(GetServerFactory, g_pBotManager, IBotManager, INTERFACEVERSION_PLAYERBOTMANAGER)
	GET_V_IFACE_ANY(GetServerFactory, servertools, IServerTools, VSERVERTOOLS_INTERFACE_VERSION)

#ifndef __linux__
	GET_V_IFACE_CURRENT(GetEngineFactory,debugoverlay, IVDebugOverlay, VDEBUG_OVERLAY_INTERFACE_VERSION)
#endif
	GET_V_IFACE_ANY(GetServerFactory, servergamedll, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL)
	GET_V_IFACE_ANY(GetServerFactory, gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS)

	gpGlobals = ismm->GetCGlobals();

	META_LOG(g_PLAPI, "Starting plugin.");
	META_LOG(g_PLAPI, "RCBot2 build stamp: " __DATE__ " " __TIME__);

	/* Load the VSP listener.  This is usually needed for IServerPluginHelpers. */
	ismm->AddListener(this, this);
	if ((vsp_callbacks = ismm->GetVSPInfo(nullptr)) == nullptr)
	{
		ismm->EnableVSPListener();
	}

	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, LevelInit, server, this, &RCBotPluginMeta::Hook_LevelInit, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, ServerActivate, server, this, &RCBotPluginMeta::Hook_ServerActivate, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, GameFrame, server, this, &RCBotPluginMeta::Hook_GameFrame, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, LevelShutdown, server, this, &RCBotPluginMeta::Hook_LevelShutdown, false);
	SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientActive, gameclients, this, &RCBotPluginMeta::Hook_ClientActive, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientDisconnect, gameclients, this, &RCBotPluginMeta::Hook_ClientDisconnect, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientPutInServer, gameclients, this, &RCBotPluginMeta::Hook_ClientPutInServer, true);
	SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientConnect, gameclients, this, &RCBotPluginMeta::Hook_ClientConnect, false);
	SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientCommand, gameclients, this, &RCBotPluginMeta::Hook_ClientCommand, false);
	//Hook FireEvent to our function - unstable for TF2? [APG]RoboCop[CL]
	SH_ADD_HOOK_MEMFUNC(IGameEventManager2, FireEvent, gameevents, this, &RCBotPluginMeta::FireGameEvent, false);
	SH_ADD_HOOK_MEMFUNC(IVEngineServer, UserMessageBegin, engine, this, &RCBotPluginMeta::Hook_UserMessageBegin, true);
	SH_ADD_HOOK_MEMFUNC(IVEngineServer, MessageEnd, engine, this, &RCBotPluginMeta::Hook_MessageEnd, false);

#if SOURCE_ENGINE >= SE_ORANGEBOX
	g_pCVar = icvar;
	ConVar_Register(0, &s_BaseAccessor);
#else
	ConCommandBaseMgr::OneTimeInit(&s_BaseAccessor);
#endif

#if SOURCE_ENGINE!=SE_DARKMESSIAH
	// read loglevel from startup param for early logging
	ConVarRef rcbot_loglevel("rcbot_loglevel");
	rcbot_loglevel.SetValue(CommandLine()->ParmValue("+rcbot_loglevel", rcbot_loglevel.GetInt()));
#endif

	// Read Signatures and Offsets
	CBotGlobals::initModFolder();
	CBotGlobals::readRCBotFolder();

	char filename[512];
	// Load RCBOT2 hook data
	CBotGlobals::buildFileName(filename, "hookinfo", BOT_CONFIG_FOLDER, "ini");

	std::fstream fp(filename, std::fstream::in);

	CRCBotKeyValueList kvl;

	if (fp)
		kvl.parseFile(fp);

	void *gameServerFactory = reinterpret_cast<void*>(ismm->GetServerFactory(false));

	int val;

#if defined(_WIN64) || defined(_WIN32)
	if (kvl.getInt("runplayermove_dods_win", &val))
		rcbot_runplayercmd_dods.SetValue(val);
	if (kvl.getInt("gamerules_win", &val))
		rcbot_gamerules_offset.SetValue(val);
	if (kvl.getInt("runplayermove_synergy_win", &val))
		rcbot_runplayercmd_syn.SetValue(val);
	if (kvl.getInt("getdatadescmap_win", &val))
		rcbot_datamap_offset.SetValue(val);
#else
	if (kvl.getInt("runplayermove_dods_linux", &val))
		rcbot_runplayercmd_dods.SetValue(val);
	if (kvl.getInt("runplayermove_synergy_linux", &val))
		rcbot_runplayercmd_syn.SetValue(val);
	if (kvl.getInt("getdatadescmap_linux", &val))
		rcbot_datamap_offset.SetValue(val);
#endif

#if SOURCE_ENGINE == SE_DODS
	// CDODPlayer vtable slots of the weapon guard (server.dll+0x2a2ad4)
#if defined(_WIN64) || defined(_WIN32)
	if (kvl.getInt("weapon_guard_dods_win", &val))
		rcbot_weapon_guard_dods.SetValue(val);
	if (kvl.getInt("weapon_guard_dods_b_win", &val))
		rcbot_weapon_guard_dods_b.SetValue(val);
#else
	if (kvl.getInt("weapon_guard_dods_linux", &val))
		rcbot_weapon_guard_dods.SetValue(val);
	if (kvl.getInt("weapon_guard_dods_b_linux", &val))
		rcbot_weapon_guard_dods_b.SetValue(val);
#endif
#endif

	g_pGameRules_Obj = new CGameRulesObject(kvl, gameServerFactory);
	g_pGameRules_Create_Obj = new CCreateGameRulesObject(kvl, gameServerFactory);

	if (fp)
		fp.close();

	if (!CBotGlobals::gameStart())
	{
		snprintf(error, maxlen, "Game mod not recognized (gamedir: \"%s\")", CBotGlobals::modFolder());
		return false;
	}

	CBotMod *pMod = CBotGlobals::getCurrentMod(); // `*pMod` Unused? [APG]RoboCop[CL]

#ifdef OVERRIDE_RUNCMD
	// TODO figure out a more robust gamedata fix instead of vtable
	#if SOURCE_ENGINE == SE_DODS
	SH_MANUALHOOK_RECONFIGURE(MHook_PlayerRunCmd, rcbot_runplayercmd_dods.GetInt(), 0, 0);
	#elif SOURCE_ENGINE == SE_SDK2013
	if(pMod->getModId() == MOD_SYNERGY)
	{
		SH_MANUALHOOK_RECONFIGURE(MHook_PlayerRunCmd, rcbot_runplayercmd_syn.GetInt(), 0, 0);
	}
#endif

#endif

	ENGINE_CALL(LogPrint)("All hooks started!\n");

	//MathLib_Init( 2.2f, 2.2f, 0.0f, 2.0f );
	//ConVar_Register( 0 );
	//InitCVars( interfaceFactory ); // register any cvars we have defined

	std::srand( static_cast<unsigned>(time(nullptr)) );  // initialize the random seed
	MTRand_int32::seed( static_cast<unsigned>(time(nullptr)) );

	// Find the RCBOT2 Path from metamod VDF
	extern IFileSystem* filesystem;
	KeyValues* mainkv = new KeyValues("metamodplugin");

	const char* rcbot2path; //Unused? [APG]RoboCop[CL]
	logger->Log(LogLevel::INFO, "Reading rcbot2 path from VDF...");

	mainkv->LoadFromFile(filesystem, "addons/metamod/rcbot2.vdf", "MOD");
	KeyValues* temp = mainkv->FindKey("Metamod Plugin");

	if (temp)
		rcbot2path = temp->GetString("rcbot2path", "\0");

	mainkv->deleteThis(); //mainkv possible redundant? [APG]RoboCop[CL]
	mainkv = temp; // Memory leak fix [APG]RoboCop[CL]

	//eventListener2 = new CRCBotEventListener();

	// Initialize bot variables
	CBotProfiles::setupProfiles();


	//CBotEvents::setupEvents();
	CWaypointTypes::setup();
	CWaypoints::setupVisibility();

	CBotConfigFile::reset();	
	CBotConfigFile::load();

	CBotMenuList::setupMenus();

	//CRCBotPlugin::ShowLicense();	

	//RandomSeed((unsigned)time(NULL));

	CClassInterface::init();

#if SOURCE_ENGINE == SE_DODS
	// [crashfix] arm the DOD:S weapon guard (server.dll+0x2a2ad4). Both netprops
	// must have resolved: without m_hOwner we cannot tell "owner is gone" from
	// "offset lookup failed", and clearing m_hActiveWeapon on every call would
	// strip every player of their weapon.
	//
	// Two CDODPlayer slots are hooked, both of which dispatch the active
	// weapon's vfunc0x3fc (the crashing one):
	//   slot 376 (0x5e0) - confirmed crash path (minidump 2026-11-12)
	//   slot 348 (0x570) - reached from CDODPlayer::vfunc0x56c
	{
		const int iSlot = rcbot_weapon_guard_dods.GetInt();
		const int iSlotB = rcbot_weapon_guard_dods_b.GetInt();
		const unsigned iOwnerOffset = CClassInterface::getWeaponOwnerOffset();
		const unsigned iActiveOffset = CClassInterface::getActiveWeaponOffset();

		if ( iOwnerOffset > 0 && iActiveOffset > 0 )
		{
			s_bDodWeaponGuardEnabled = false;
			s_bDodWeaponGuardEnabledB = false;

			if ( iSlot > 0 )
			{
				SH_MANUALHOOK_RECONFIGURE(MHook_DodWeaponGuard, iSlot, 0, 0);
				s_bDodWeaponGuardEnabled = true;
			}

			if ( iSlotB > 0 && iSlotB != iSlot )
			{
				SH_MANUALHOOK_RECONFIGURE(MHook_DodWeaponGuardB, iSlotB, 0, 0);
				s_bDodWeaponGuardEnabledB = true;
			}

			if ( s_bDodWeaponGuardEnabled || s_bDodWeaponGuardEnabledB )
			{
				logger->Log(LogLevel::WARN,
					"[crashfix] DOD:S weapon guard ON: slots %d%s%d, m_hOwner=%u, m_hActiveWeapon=%u",
					iSlot,
					s_bDodWeaponGuardEnabledB ? ", " : "",
					s_bDodWeaponGuardEnabledB ? iSlotB : 0,
					iOwnerOffset, iActiveOffset);
			}
			else
			{
				logger->Log(LogLevel::ERROR,
					"[crashfix] DOD:S weapon guard OFF: no vtable slot configured (slot=%d slot_b=%d)",
					iSlot, iSlotB);
			}
		}
		else
		{
			s_bDodWeaponGuardEnabled = false;
			s_bDodWeaponGuardEnabledB = false;
			logger->Log(LogLevel::ERROR,
				"[crashfix] DOD:S weapon guard OFF: slot=%d/%d m_hOwner=%u m_hActiveWeapon=%u",
				iSlot, iSlotB, iOwnerOffset, iActiveOffset);
		}
	}
#endif

	RCBOT2_Cvar_setup(g_pCVar);

	// Bot Quota Settings
	char bq_line[128];

	int bot_count = 0;
	int human_count = 0;

	for (int& m_iTargetBot : m_iTargetBots)
	{
		m_iTargetBot = 0;
	}

	CBotGlobals::buildFileName(filename, "bot_quota", BOT_CONFIG_FOLDER, "ini");
	fp = std::fstream(filename, std::fstream::in);

	std::memset(bq_line, 0, sizeof(bq_line));

	if (fp) {
		while (fp.getline(bq_line, sizeof(bq_line))) {
			if (bq_line[0] == '#')
				continue;

			for (char& i : bq_line)
			{
				if (i == '\0')
					break;

				if (!isdigit(i))
					i = ' ';
			}

			if (std::sscanf(bq_line, "%d %d", &human_count, &bot_count) == 2) {
				if (human_count < 0 || human_count > RCBOT_MAXPLAYERS) {
					logger->Log(LogLevel::WARN, "Bot Quota - Invalid Human Count %d", human_count);
					continue;
				}

				if (bot_count < 0 || bot_count > RCBOT_MAXPLAYERS) {
					logger->Log(LogLevel::WARN, "Bot Quota - Invalid Bot Count %d", bot_count);
					continue;
				}

				if (human_count < RCBOT_MAXPLAYERS) { // Ensure human_count is within bounds
					m_iTargetBots[human_count] = bot_count;
					logger->Log(LogLevel::INFO, "Bot Quota - Humans: %d, Bots: %d", human_count, bot_count);
				}
			}
		}
	}

	return true;
}

bool RCBotPluginMeta::FireGameEvent(IGameEvent* pevent, bool bDontBroadcast)
{
#ifdef RCBOT_VPROF_ENABLED
	VPROF_BUDGET("RCBotPluginMeta::FireGameEvent", "RCBot2")
#endif // RCBOT_VPROF_ENABLED

	// Skip processing if another plugin already handled/blocked this event - [APG]RoboCop[CL]
	if (META_RESULT_STATUS >= MRES_OVERRIDE)
	{
		RETURN_META_VALUE(MRES_IGNORED, true);
	}

	// Sanity check: ensure event pointer is valid before processing - [APG]RoboCop[CL]
	if (pevent != nullptr)
	{
		CBotEvents::executeEvent(pevent, TYPE_IGAMEEVENT);
	}

	RETURN_META_VALUE(MRES_IGNORED, true);
}

bool RCBotPluginMeta::Unload(char *error, std::size_t maxlen)
{
#ifdef SM_EXT
	SM_UnloadExtension();
#endif
	
	//CBots::kickRandomBot(RCBOT_MAXPLAYERS); //breaks the bot quota system? [APG]RoboCop[CL]
	
	SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, LevelInit, server, this, &RCBotPluginMeta::Hook_LevelInit, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, ServerActivate, server, this, &RCBotPluginMeta::Hook_ServerActivate, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, GameFrame, server, this, &RCBotPluginMeta::Hook_GameFrame, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, LevelShutdown, server, this, &RCBotPluginMeta::Hook_LevelShutdown, false);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameClients, ClientActive, gameclients, this, &RCBotPluginMeta::Hook_ClientActive, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameClients, ClientDisconnect, gameclients, this, &RCBotPluginMeta::Hook_ClientDisconnect, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameClients, ClientPutInServer, gameclients, this, &RCBotPluginMeta::Hook_ClientPutInServer, true);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameClients, ClientConnect, gameclients, this, &RCBotPluginMeta::Hook_ClientConnect, false);
	SH_REMOVE_HOOK_MEMFUNC(IServerGameClients, ClientCommand, gameclients, this, &RCBotPluginMeta::Hook_ClientCommand, false);
	// Unhook the FireEvent hook to avoid potential crashes during plugin unload and for dod_broadcast_audio? [APG]RoboCop[CL]
	SH_REMOVE_HOOK_MEMFUNC(IGameEventManager2, FireEvent, gameevents, this, &RCBotPluginMeta::FireGameEvent, false);
	SH_REMOVE_HOOK_MEMFUNC(IVEngineServer, UserMessageBegin, engine, this, &RCBotPluginMeta::Hook_UserMessageBegin, true);
	SH_REMOVE_HOOK_MEMFUNC(IVEngineServer, MessageEnd, engine, this, &RCBotPluginMeta::Hook_MessageEnd, false);

	//SH_REMOVE_MANUALHOOK(MHook_PlayerRunCmd, player_vtable, SH_STATIC(Hook_Function2), false);

	// if another instance is running dont run through this
	//if ( !bInitialised )
	//	return;
	
	CBots::freeAllMemory();
	CStrings::freeAllMemory();
	CBotMods::freeMemory();
	CAccessClients::freeMemory();
	CBotEvents::freeMemory();
	CWaypoints::freeMemory();
	CWaypointTypes::freeMemory();
	CBotProfiles::deleteProfiles();
	CWeapons::freeMemory();
	CBotMenuList::freeMemory();
	//unloadSignatures();

	//UnhookPlayerRunCommand();
	//UnhookGiveNamedItem();

	//ConVar_Unregister();

	//if ( gameevents )
	//	gameevents->RemoveListener(this);

	ConVar_Unregister( );

	return true;
}

void RCBotPluginMeta::OnVSPListening(IServerPluginCallbacks *iface)
{
	vsp_callbacks = iface;
}

void RCBotPluginMeta::Hook_ServerActivate(edict_t *pEdictList, const int edictCount, const int clientMax)
{
	META_LOG(g_PLAPI, "ServerActivate() called: edictCount = %d, clientMax = %d", edictCount, clientMax);

	CAccessClients::load();

	CBotGlobals::setClientMax(clientMax);
}

void RCBotPluginMeta::AllPluginsLoaded()
{
	/* This is where we'd do stuff that relies on the mod or other plugins 
	 * being initialized (for example, cvars added and events registered).
	 */
#ifdef SM_EXT
	BindToSourcemod();
#endif
}

#ifdef SM_EXT
void* RCBotPluginMeta::OnMetamodQuery(const char* iface, int *ret) {
	if (std::strcmp(iface, SOURCEMOD_NOTICE_EXTENSIONS) == 0) {
		BindToSourcemod();
	}
	
	if (ret != nullptr) {
		*ret = IFACE_OK;
	}
	
	return nullptr;
}
#endif

void RCBotPluginMeta::Hook_ClientActive(edict_t *pEntity, const bool bLoadGame)
{
	META_LOG(g_PLAPI, "Hook_ClientActive(%d, %d)", IndexOfEdict(pEntity), bLoadGame);

	CClients::clientActive(pEntity);
}

#if SOURCE_ENGINE >= SE_ORANGEBOX
void RCBotPluginMeta::Hook_ClientCommand(edict_t *pEntity, const CCommand &args)
#else
void RCBotPluginMeta::Hook_ClientCommand(edict_t *pEntity)
#endif
{
#ifdef RCBOT_VPROF_ENABLED
	VPROF_BUDGET("RCBotPluginMeta::Hook_ClientCommand", "RCBot2")
#endif // RCBOT_VPROF_ENABLED

	static CBotMod *pMod = nullptr;

#if SOURCE_ENGINE <= SE_DARKMESSIAH
	CCommand args;
#endif

	const char *pcmd = args.Arg(0);

	if (!pEntity || pEntity->IsFree())
	{
		return;
	}

	CClient *pClient = CClients::get(pEntity);

	// is bot command?
	if ( CBotGlobals::m_pCommands->isCommand(pcmd) )
	{		
		// create shifted command list
		BotCommandArgs argList;
		for (unsigned i = 1; i <= static_cast<unsigned>(args.ArgC()); i++) {
			argList.emplace_back(args.Arg(static_cast<int>(i)));
		}
		const eBotCommandResult iResult = CBotGlobals::m_pCommands->execute(pClient, argList);

		if ( iResult == COMMAND_ACCESSED )
		{
			// ok
		}
		else if ( iResult == COMMAND_REQUIRE_ACCESS )
		{
			CBotGlobals::botMessage(pEntity,0,"You do not have access to this command");
		}
		else if ( iResult == COMMAND_NOT_FOUND )
		{
			CBotGlobals::botMessage(pEntity,0,"bot command not found");	
		}
		else if ( iResult == COMMAND_ERROR )
		{
			CBotGlobals::botMessage(pEntity,0,"bot command returned an error");	
		}

		RETURN_META(MRES_SUPERCEDE);
	}
	if ( std::strncmp(pcmd,"menuselect",10) == 0 ) // menu command
	{
		if ( pClient->isUsingMenu() )
		{
			int iCommand = std::atoi(args.Arg(1));

			// format is 1.2.3.4.5.6.7.8.9.0
			if ( iCommand == 0 )
				iCommand = 9;
			else
				iCommand --;

			pClient->getCurrentMenu()->selectedMenu(pClient,iCommand);
		}
	}

	// command capturing
	pMod = CBotGlobals::getCurrentMod();

	// capture some client commands e.g. voice commands
	pMod->clientCommand(pEntity,args.ArgC(),pcmd,args.Arg(1),args.Arg(2));

	RETURN_META(MRES_IGNORED); 
}

bool RCBotPluginMeta::Hook_ClientConnect(edict_t *pEntity,
									const char *pszName,
									const char *pszAddress,
									char *reject,
									int maxrejectlen)
{
	META_LOG(g_PLAPI, R"(Hook_ClientConnect(%d, "%s", "%s"))", IndexOfEdict(pEntity), pszName, pszAddress);

	CClients::init(pEntity);

	return true;
}

void RCBotPluginMeta::Hook_ClientPutInServer(edict_t *pEntity, char const* playername)
{
	CBaseEntity *pEnt = servergameents->EdictToBaseEntity(pEntity); //`*pEnt` Unused? [APG]RoboCop[CL]
	constexpr bool is_Rcbot = false; //`is_Rcbot` Unused? [APG]RoboCop[CL]

	if ( CClient *pClient = CClients::clientConnected(pEntity) )
	{
		if ( !engine->IsDedicatedServer() )
		{
			if ( CClients::noListenServerClient() )
			{
				// give listenserver client all access to bot commands
				CClients::setListenServerClient(pClient);
				pClient->setAccessLevel(CMD_ACCESS_ALL);
				pClient->resetMenuCommands();
			}
		}
	}

	CBotMod *pMod = CBotGlobals::getCurrentMod();

	pMod->playerSpawned(pEntity);

#ifdef OVERRIDE_RUNCMD
	// Don't add the PlayerRunCmd vtable hook for FF - the vtable offset is
	// not configured and hooking at offset 0 would hook the wrong function.
	// FF bots use m_pController->RunPlayerMove() directly instead.
	if ( pEnt && !CBotGlobals::isMod(MOD_FF) )
	{
		SH_ADD_MANUALHOOK_MEMFUNC(MHook_PlayerRunCmd, pEnt, this, &RCBotPluginMeta::Hook_PlayerRunCmd, false);
	}
#endif

#if SOURCE_ENGINE == SE_DODS
	// [crashfix] the victim of server.dll+0x2a2ad4 can be a human, so the
	// guard is installed for every client (bots too)
	if ( pEnt && (s_bDodWeaponGuardEnabled || s_bDodWeaponGuardEnabledB) )
	{
		// [crashfix] capture the pre-hook entries so later reads can tell
		// "SourceHook stub installed" from "original server.dll target"
		void **pVtblPre = *reinterpret_cast<void ***>(pEnt);

		if ( !s_bOrigVtblCaptured && pVtblPre != nullptr )
		{
			s_pOrigVtbl347 = pVtblPre[347];
			s_pOrigVtbl348 = pVtblPre[348];
			s_pOrigVtbl376 = pVtblPre[376];
			s_bOrigVtblCaptured = true;
		}
	}

	if ( pEnt && s_bDodWeaponGuardEnabled )
	{
		SH_ADD_MANUALHOOK_MEMFUNC(MHook_DodWeaponGuard, pEnt, this, &RCBotPluginMeta::Hook_DodWeaponGuard, false);
	}

	if ( pEnt && s_bDodWeaponGuardEnabledB )
	{
		SH_ADD_MANUALHOOK_MEMFUNC(MHook_DodWeaponGuardB, pEnt, this, &RCBotPluginMeta::Hook_DodWeaponGuard, false);
	}

	// [crashfix] log what the instance's vtable looks like AFTER the ADD:
	// a slot entry equal to the captured original means the hook is NOT on
	// this instance (H1), a different value is the SourceHook stub
	if ( pEnt && (s_bDodWeaponGuardEnabled || s_bDodWeaponGuardEnabledB) )
	{
		void **pVtbl = *reinterpret_cast<void ***>(pEnt);

		if ( pVtbl != nullptr )
		{
			// META_LOG (see the guard probe comment): must reach the engine
			// log file for every client, not just the first one
			META_LOG(g_PLAPI,
				"[crashfix] install: ent %d vtbl=%p s347=%p s348=%p s376=%p | orig347=%p orig348=%p orig376=%p",
				IndexOfEdict(pEntity), pVtbl,
				pVtbl[347], pVtbl[348], pVtbl[376],
				s_pOrigVtbl347, s_pOrigVtbl348, s_pOrigVtbl376);
		}
	}
#endif
}

void RCBotPluginMeta::Hook_ClientDisconnect(edict_t *pEntity)
{
	CBaseEntity *pEnt = servergameents->EdictToBaseEntity(pEntity); //`*pEnt` Unused? [APG]RoboCop[CL]

#ifdef OVERRIDE_RUNCMD
	if ( pEnt && !CBotGlobals::isMod(MOD_FF) )
	{
		SH_REMOVE_MANUALHOOK_MEMFUNC(MHook_PlayerRunCmd, pEnt, this, &RCBotPluginMeta::Hook_PlayerRunCmd, false);
	}
#endif

#if SOURCE_ENGINE == SE_DODS
	if ( pEnt && s_bDodWeaponGuardEnabled )
	{
		SH_REMOVE_MANUALHOOK_MEMFUNC(MHook_DodWeaponGuard, pEnt, this, &RCBotPluginMeta::Hook_DodWeaponGuard, false);
	}

	if ( pEnt && s_bDodWeaponGuardEnabledB )
	{
		SH_REMOVE_MANUALHOOK_MEMFUNC(MHook_DodWeaponGuardB, pEnt, this, &RCBotPluginMeta::Hook_DodWeaponGuard, false);
	}
#endif

	CClients::clientDisconnected(pEntity);

	META_LOG(g_PLAPI, "Hook_ClientDisconnect(%d)", IndexOfEdict(pEntity));
}

void RCBotPluginMeta::Hook_GameFrame(const bool simulating)
{
#ifdef RCBOT_VPROF_ENABLED
	VPROF_BUDGET("RCBotPluginMeta::Hook_GameFrame", "RCBot2")
#endif // RCBOT_VPROF_ENABLED

	/**
	 * simulating:
	 * ***********
	 * true  | game is ticking
	 * false | game is not ticking
	 */

#if SOURCE_ENGINE == SE_DODS
	// [crashfix] A/B heartbeat: one line every 10s while the guard is on,
	// pairing the handler counters with the per-instance install state, so a
	// run can show "hooks are on every client AND the handler fires" (guard
	// works) vs "hooks are on but the crash chain never entered" (H2)
	if ( (s_bDodWeaponGuardEnabled || s_bDodWeaponGuardEnabledB) &&
		gpGlobals != nullptr && s_bOrigVtblCaptured &&
		gpGlobals->curtime >= s_flNextInstallProbe )
	{
		s_flNextInstallProbe = gpGlobals->curtime + 10.0f;

		unsigned uClients = 0, uHooked347 = 0, uHooked348 = 0, uHooked376 = 0, uVptrDiff = 0;
		void **pFirstVtbl = nullptr;

		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			edict_t *pEd = engine->PEntityOfEntIndex(i);

			if ( pEd == nullptr || pEd->IsFree() )
				continue;

			CBaseEntity *pBase = servergameents->EdictToBaseEntity(pEd);

			if ( pBase == nullptr )
				continue;

			void **pV = *reinterpret_cast<void ***>(pBase);

			if ( pV == nullptr )
				continue;

			++uClients;

			if ( pFirstVtbl == nullptr )
				pFirstVtbl = pV;
			else if ( pV != pFirstVtbl )
				++uVptrDiff;

			if ( pV[347] != s_pOrigVtbl347 ) ++uHooked347;
			if ( pV[348] != s_pOrigVtbl348 ) ++uHooked348;
			if ( pV[376] != s_pOrigVtbl376 ) ++uHooked376;
		}

		// META_LOG: the heartbeat is the primary H1/H2 evidence and must
		// keep flowing for the whole session (see the guard probe comment)
		META_LOG(g_PLAPI,
			"[crashfix] install probe: clients=%u hooked347=%u hooked348=%u hooked376=%u vptrDiff=%u | calls=%u cleared=%u ret_active_invalid=%u ret_owner_is_player=%u ret_no_prop=%u | vtbl348=%p orig348=%p",
			uClients, uHooked347, uHooked348, uHooked376, uVptrDiff,
			s_uGuardCalls, s_uGuardCleared, s_uGuardRetActiveInvalid,
			s_uGuardRetOwnerIsPlayer, s_uGuardRetNoProp,
			pFirstVtbl ? pFirstVtbl[348] : nullptr, s_pOrigVtbl348);
	}
#endif

	if ( simulating && CBotGlobals::IsMapRunning() )
	{
		static CBotMod *currentmod;

		CBots::botThink();
		CClients::clientThink();

		if ( CWaypoints::getVisiblity()->needToWorkVisibility() )
		{
			CWaypoints::getVisiblity()->workVisibility();
		}

		// Profiling
#ifdef _DEBUG
		if ( CClients::clientsDebugging(BOT_DEBUG_PROFILE) )
		{
			CProfileTimers::updateAndDisplay();
		}
#endif

		// Config Commands
		CBotConfigFile::doNextCommand();
		currentmod = CBotGlobals::getCurrentMod();

		currentmod->modFrame();

		// Bot Quota
		if (rcbot_bot_quota_interval.GetFloat() > 0.0) {
			BotQuotaCheck();
		}
	}
}

void RCBotPluginMeta::BotQuotaCheck() {
	// this is configured with config/bot_quota.ini
	if (rcbot_bot_quota_interval.GetFloat() <= 0.0) {
		return;
	}

	if (m_fBotQuotaTimer < 1.0f) {
		m_fBotQuotaTimer = engine->Time() + 10.0f; // Sleep 10 seconds
	}

	if (m_fBotQuotaTimer < engine->Time() - rcbot_bot_quota_interval.GetFloat()) {
		m_fBotQuotaTimer = engine->Time();

		// Change Notification
		bool notify = false;

		// Current Bot Count
		int bot_count = 0;
		int human_count = 0;

		// Count Players
		for (int i = 0; i < RCBOT_MAXPLAYERS; ++i) {
			const CClient* client = CClients::get(i);
			const CBot* bot = CBots::get(i);

			if (bot != nullptr && bot->getEdict() != nullptr && bot->inUse()) {
				IPlayerInfo *p = playerinfomanager->GetPlayerInfo(bot->getEdict());

				if (p->IsConnected() && p->IsFakeClient() && !p->IsHLTV()) {
					bot_count++;
				}
			}

			if (client->getPlayer() != nullptr && client->isUsed()) {
				IPlayerInfo* p = playerinfomanager->GetPlayerInfo(client->getPlayer());

				if (p->IsConnected() && !p->IsFakeClient() && !p->IsHLTV()) {
					if (rcbot_ignore_spectators.GetBool()) {
						if ( CClassInterface::getTeam(client->getPlayer()) >= 2 )
							human_count++;
					} else
						human_count++;
				}
			}
		}

		if (human_count >= RCBOT_MAXPLAYERS) {
			human_count = 0;
		}

		// Get Bot Quota
		int bot_target = m_iTargetBots[human_count];
		const int max_bots = CBots::getMaxBots();
		const int min_bots = CBots::getMinBots();

		// Use min and max as ceiling and floor
		if ((max_bots > -1) && (bot_target > max_bots)) {
			bot_target = max_bots;
		} else if ((min_bots > -1) && (bot_target < min_bots)) {
			bot_target = min_bots;
		}

		// Change Bot Quota
		// In MVM mode, don't kick RCBots via quota - TF2 manages BLU team slots
		const bool bIsMVM = CTeamFortress2Mod::isMapType(TF_MAP_MVM);

		if (bot_count > bot_target && !bIsMVM) {
			if (rcbot_nonrandom_kicking.GetBool()) {
				CBots::kickChosenBot(static_cast<unsigned>(bot_count - bot_target));
			} else {
				CBots::kickRandomBot(static_cast<unsigned>(bot_count - bot_target));
			}
			notify = true;
		}
		else if ((bot_target > bot_count) && ( CBots::getAddKickBotTime() < engine->Time() )) {
			const int bot_diff = bot_target - bot_count;

			for (int i = 0; i < bot_diff; ++i) {
				CBots::createBot("", "", "");
				if (rcbot_addbottime.GetFloat() > 0.0)
					break;
			}

			notify = true;
		}

		if (notify) {
			char chatmsg[128];
			snprintf(chatmsg, sizeof(chatmsg), "[Bot Quota] Humans: %d, Bots: %d", human_count, bot_target);
			logger->Log(LogLevel::INFO, chatmsg);
			// RCBotPluginMeta::BroadcastTextMessage(chatmsg);
		}
	}
}

bool RCBotPluginMeta::Hook_LevelInit(const char *pMapName,
								char const *pMapEntities,
								char const *pOldLevel,
								char const *pLandmarkName,
								bool loadGame,
								bool background)
{
	META_LOG(g_PLAPI, "Hook_LevelInit(%s)", pMapName);

		//CClients::initall();
	// Must set this
	CBotGlobals::setMapName(pMapName);

	logger->Log(LogLevel::INFO, "Level \"%s\" has been loaded", pMapName);

	CWaypoints::precacheWaypointTexture();

	CWaypointDistances::reset();

	CProfileTimers::reset();

	CWaypoints::init();
	CWaypoints::load();

	CBotGlobals::setMapRunning(true);
	CBotConfigFile::reset();
	
	if ( mp_teamplay.IsValid() )
		CBotGlobals::setTeamplay(mp_teamplay.GetBool());
	else
		CBotGlobals::setTeamplay(false);

	CBotEvents::setupEvents();

	CBots::mapInit();

	if ( CBotMod *pMod = CBotGlobals::getCurrentMod() )
		pMod->mapInit();

	CBotSquads::FreeMemory();

	CClients::setListenServerClient(nullptr);

	// Setup game rules
	extern void **g_pGameRules; //Unused? [APG]RoboCop[CL]

	if (g_pGameRules_Obj && g_pGameRules_Obj->found())
	{
		g_pGameRules = g_pGameRules_Obj->getGameRules();
	}
	else if (g_pGameRules_Create_Obj && g_pGameRules_Create_Obj->found())
	{
		g_pGameRules = g_pGameRules_Create_Obj->getGameRules();
	}

	return true;
}

void RCBotPluginMeta::Hook_LevelShutdown()
{
	META_LOG(g_PLAPI, "Hook_LevelShutdown()");

	CClients::initall();
	CWaypointDistances::save();

	CBots::freeMapMemory();	
	CWaypoints::init();

	CBotGlobals::setMapRunning(false);
	CBotEvents::freeMemory();
}

bool RCBotPluginMeta::Pause(char *error, std::size_t maxlen)
{
	return true;
}

bool RCBotPluginMeta::Unpause(char *error, std::size_t maxlen)
{
	return true;
}

const char *RCBotPluginMeta::GetLicense()
{
	return "GPL General Public License";
}

const char *RCBotPluginMeta::GetVersion()
{
	return build_info::short_version;
}

const char *RCBotPluginMeta::GetDate()
{
	return build_info::date;
}

const char *RCBotPluginMeta::GetLogTag()
{
	return "RCBOT2";
}

const char *RCBotPluginMeta::GetAuthor()
{
	return build_info::authors;
}

const char *RCBotPluginMeta::GetDescription()
{
	return "Bot for HL2DM, TF2 and DOD:S";
}

const char *RCBotPluginMeta::GetName()
{
	return "RCBot2";
}

const char *RCBotPluginMeta::GetURL()
{
	return build_info::url;
}

#ifdef SM_EXT
void RCBotPluginMeta::BindToSourcemod()
{
	char error[256];
	if (!SM_LoadExtension(error, sizeof(error))) {
		char message[512];
		snprintf(message, sizeof(message), "Could not load as a SourceMod extension: %s\n", error);
		engine->LogPrint(message);
	}
}
#endif
