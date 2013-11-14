/* Copyright (C) 2003-2013 Runtime Revolution Ltd.

This file is part of LiveCode.

LiveCode is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License v3 as published by the Free
Software Foundation.

LiveCode is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with LiveCode.  If not see <http://www.gnu.org/licenses/>.  */

#include "prefix.h"

#include "globdefs.h"
#include "filedefs.h"
#include "objdefs.h"
#include "parsedef.h"
#include "mcio.h"

#include "execpt.h"
#include "dispatch.h"
#include "stack.h"
#include "tooltip.h"
#include "card.h"
#include "field.h"
#include "button.h"
#include "image.h"
#include "aclip.h"
#include "vclip.h"
#include "stacklst.h"
#include "mcerror.h"
#include "hc.h"
#include "util.h"
#include "param.h"
#include "debug.h"
#include "statemnt.h"
#include "funcs.h"
#include "magnify.h"
#include "sellst.h"
#include "undolst.h"
#include "styledtext.h"
#include "property.h"
#include "osspec.h"

#include "system.h"
#include "globals.h"
#include "license.h"
#include "mode.h"
#include "revbuild.h"
#include "deploy.h"
#include "capsule.h"
#include "player.h"

#if defined(_WINDOWS_DESKTOP)
#include "w32prefix.h"
#elif defined(_MAC_DESKTOP)
#include "osxprefix.h"
#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Globals specific to STANDALONE mode
//

// The MCDeployProjectInfo structure is generated by the internal 'deploy'
// command when a standalone is built. It contains all the information
// pertaining to the built application, and is used at runtime to locate, validate
// and launch the user's standalone application.
//
// The structure is placed in a specific executable 'section' which the deploy
// command can locate and modify.
//

struct MCCapsuleInfo
{
	uint32_t size;
	uint32_t data[3];
};

#if defined(_WINDOWS)
#pragma section(".project", read, discard)
__declspec(allocate(".project")) volatile MCCapsuleInfo MCcapsule = {0};
#elif defined(_LINUX)
__attribute__((section(".project"))) volatile MCCapsuleInfo MCcapsule = {0};
#elif defined(_MACOSX)
__attribute__((section("__PROJECT,__project"))) volatile MCCapsuleInfo MCcapsule = {0};
#elif defined(TARGET_SUBPLATFORM_IPHONE)
__attribute__((section("__PROJECT,__project"))) volatile MCCapsuleInfo MCcapsule = {0};
#elif defined(TARGET_SUBPLATFORM_ANDROID)
__attribute__((section(".project"))) volatile MCCapsuleInfo MCcapsule = {0};
#elif defined(TARGET_PLATFORM_MOBILE)
MCCapsuleInfo MCcapsule = {0};
#endif

MCLicenseParameters MClicenseparameters =
{
	NULL, NULL, NULL, kMCLicenseClassNone, 0,
	0, 0, 0, 0,
	0,
	NULL,
};

// We don't include error string in this mode
const char *MCparsingerrors = "";
const char *MCexecutionerrors = "";

////////////////////////////////////////////////////////////////////////////////
//
//  Property tables specific to STANDALONE mode
//

MCPropertyInfo MCObject::kModeProperties[] =
{
};

MCObjectPropertyTable MCObject::kModePropertyTable =
{
	nil,
	0,
	nil,
};

MCPropertyInfo MCStack::kModeProperties[] =
{
};

MCObjectPropertyTable MCStack::kModePropertyTable =
{
	nil,
	0,
	nil,
};

MCPropertyInfo MCProperty::kModeProperties[] =
{
};

MCPropertyTable MCProperty::kModePropertyTable =
{
	0,
	nil,
};

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of MCDispatch::startup method for STANDALONE mode.
//

extern IO_stat readheader(IO_handle& stream, char *version);
extern void send_startup_message(bool p_do_relaunch = true);

extern void add_simulator_redirect(const char *);

// This structure contains the information we collect from reading in the
// project.
struct MCStandaloneCapsuleInfo
{
	uint32_t origin;
	
	MCStack *stack;
	bool done;
};

bool MCStandaloneCapsuleCallback(void *p_self, const uint8_t *p_digest, MCCapsuleSectionType p_type, uint32_t p_length, IO_handle p_stream)
{
	MCStandaloneCapsuleInfo *self;
	self = static_cast<MCStandaloneCapsuleInfo *>(p_self);
	
	// If we've already seen the epilogue, we are done.
	if (self -> done)
	{
		MCresult -> sets("unexpected data encountered");
		return false;
	}

	switch(p_type)
	{
	case kMCCapsuleSectionTypeEpilogue:
		self -> done = true;
		break;

	case kMCCapsuleSectionTypePrologue:
	{
		MCCapsulePrologueSection t_prologue;
		if (IO_read(&t_prologue, sizeof(t_prologue), p_stream) != IO_NORMAL)
		{
			MCresult -> sets("failed to read standalone prologue");
			return false;
		}
	}
	break;

	case kMCCapsuleSectionTypeRedirect:
	{
		char *t_redirect;
		t_redirect = new char[p_length];
		if (IO_read(t_redirect, p_length, p_stream) != IO_NORMAL)
		{
			MCresult -> sets("failed to read redirect ref");
			return false;
		}
		
#ifdef TARGET_SUBPLATFORM_IPHONE
		add_simulator_redirect(t_redirect);
#endif

		delete t_redirect;
	}
	break;
			
	case kMCCapsuleSectionTypeStack:
		if (MCdispatcher -> readstartupstack(p_stream, self -> stack) != IO_NORMAL)
		{
			MCresult -> sets("failed to read standalone stack");
			return false;
		}
		
		// MW-2012-10-25: [[ Bug ]] Make sure we set these to the main stack so that
		//   the startup script and such work.
		MCstaticdefaultstackptr = MCdefaultstackptr = self -> stack;
	break;
			
	case kMCCapsuleSectionTypeExternal:
	{
		char *t_external;
		t_external = new char[p_length];
		if (IO_read(t_external, p_length, p_stream) != IO_NORMAL)
		{
			MCresult -> sets("failed to read external ref");
			return false;
		}
		
		MCAutoStringRef t_external_str;
		/* UNCHECKED */ MCStringCreateWithCString(t_external, &t_external_str);
		if (!MCdispatcher -> loadexternal(*t_external_str))
		{
			delete t_external;
			MCresult -> sets("failed to load external");
			return false;
		}
		
		delete t_external;
	}
	break;
			
	case kMCCapsuleSectionTypeStartupScript:
	{
		char *t_script;
		t_script = new char[p_length];
		if (IO_read(t_script, p_length, p_stream) != IO_NORMAL)
		{
			MCresult -> sets("failed to read startup script");
			return false;
		}

		// Execute the startup script at this point since we have loaded
		// all stacks.
        MCAutoStringRef t_script_str;
        /* UNCHECKED */ MCStringCreateWithCString(t_script, &t_script_str);
		self -> stack -> domess(*t_script_str);
		
		delete t_script;
	}
	break;
			
	case kMCCapsuleSectionTypeAuxillaryStack:
	{
		MCStack *t_aux_stack;
		if (MCdispatcher -> readfile(NULL, NULL, p_stream, t_aux_stack) != IO_NORMAL)
		{
			MCresult -> sets("failed to read auxillary stack");
			return false;
		}
	}
	break;

	case kMCCapsuleSectionTypeDigest:
		uint8_t t_read_digest[16];
		if (IO_read(t_read_digest, 16, p_stream) != IO_NORMAL)
		{
			MCresult -> sets("failed to read standalone checksum");
			return false;
		}
		if (memcmp(t_read_digest, p_digest, 16) != 0)
		{
			MCresult -> sets("standalone checksum mismatch");
			return false;
		}
		break;
			
			
	default:
		MCresult -> sets("unrecognized section encountered");
		return false;
	}
	
	return true;
}

#ifdef _MOBILE

#ifdef TARGET_SUBPLATFORM_ANDROID
extern void MCAndroidEngineRemoteCall(const char *, const char *, void *, ...);
#endif

IO_stat MCDispatch::startup(void)
{
	startdir = MCS_getcurdir();
	enginedir = strdup(MCStringGetCString(MCcmd));
	char *eptr = strrchr(enginedir, PATH_SEPARATOR);
	if (eptr != NULL)
		*eptr = '\0';
	else
		*enginedir = '\0';
	char *openpath = strdup(MCStringGetCString(MCcmd)); //point to MCcmd string

	// set up image cache before the first stack is opened
	MCCachedImageRep::init();
	
#if !defined(_DEBUG) && !defined(_SHARK) && (defined(TARGET_SUBPLATFORM_IPHONE) || defined(TARGET_SUBPLATFORM_ANDROID))
	// This is the 'built-as-standalone' iPhone path.

	// The info structure that will be filled in while parsing the capsule.
	MCStandaloneCapsuleInfo t_info;
	memset(&t_info, 0, sizeof(MCStandaloneCapsuleInfo));

	// Create a capsule and fill with the standalone data
	MCCapsuleRef t_capsule;
	t_capsule = nil;
	if (!MCCapsuleOpen(MCStandaloneCapsuleCallback, &t_info, t_capsule))
		return IO_ERROR;
	
	if (((MCcapsule . size) & (1 << 31U)) == 0)
	{
		// Capsule is not spilled - just use the project section.
		if (!MCCapsuleFillNoCopy(t_capsule, (const void *)&MCcapsule . data[0], MCcapsule . size, true))
		{
			MCCapsuleClose(t_capsule);
			return IO_ERROR;
		}
	}
	else
	{
		// Capsule is spilled fill from:
		//   0..2044 from project section
		//   spill file
		//   rest from project section
		char *t_spill;
		t_spill = (char *)malloc(strlen(openpath) + 5);
		sprintf(t_spill, "%s.dat", openpath);
		if (!MCCapsuleFillNoCopy(t_capsule, (const void *)&MCcapsule . data, 2044, false) ||
			!MCCapsuleFillFromFile(t_capsule, t_spill, 0, false) ||
			!MCCapsuleFillNoCopy(t_capsule, (const uint8_t *)&MCcapsule . data + 2044, 2048, true))
		{
			free(t_spill);
			MCCapsuleClose(t_capsule);
			return IO_ERROR;
		}
		free(t_spill);
	}
	
	// Process the capsule
	if (!MCCapsuleProcess(t_capsule))
	{
		MCCapsuleClose(t_capsule);
		return IO_ERROR;
	}

	/* UNCHECKED */ MCStringCreateWithCString(openpath, MCcmd);
	MCdefaultstackptr = MCstaticdefaultstackptr = t_info . stack;
	MCCapsuleClose(t_capsule);

	// Work out whether we are running in the emulator or not
	bool t_is_device;
#if defined(TARGET_SUBPLATFORM_IPHONE)
#if defined(__i386__)
	t_is_device = false;
#else
	t_is_device = true;
#endif
#elif defined(TARGET_SUBPLATFORM_ANDROID)
	MCAutoStringRef t_machine_string;
	/* UNCHECKED */ MCS_getmachine(&t_machine_string);
	if (MCStringIsEqualToCString(*t_machine_string, "sdk", kMCCompareExact))
		t_is_device = false;
	else
		t_is_device = true;
#else
#error Device detection not implemented
#endif

	if (!MCquit)
	{
		t_info . stack -> extraopen(false);
	
		// Resolve parent scripts *after* we've loaded aux stacks.
		if (t_info . stack -> getextendedstate(ECS_USES_PARENTSCRIPTS))
			t_info . stack -> resolveparentscripts();
		
		MCscreen->resetcursors();
		MCImage::init();
	}
	
	// MW-2010-12-18: Startup message / stack init now down in 'main'

#else
	// This is the debug or Android path.
	
	IO_handle t_stream;

	// In debug mode, we (for now) load a fixed stack
#if defined(TARGET_SUBPLATFORM_ANDROID)
	extern IO_handle android_get_mainstack_stream();
	t_stream = android_get_mainstack_stream();
#else
	MCAutoStringRef t_path;
	MCStringFormat(&t_path, "%.*s/iphone_test.rev", strrchr(MCStringGetCString(MCcmd), '/') - MCStringGetCString(MCcmd), MCStringGetCString(MCcmd));
	t_stream = MCS_open(*t_path, kMCSOpenFileModeRead, False, False, 0);
#endif

	if (t_stream == NULL)
		return IO_ERROR;

	MCStack *t_stack;
	if (readstartupstack(t_stream, t_stack) != IO_NORMAL)
		return IO_ERROR;

	MCS_close(t_stream);

	/* UNCHECKED */ MCStringCreateWithCString(openpath, MCcmd);
	MCdefaultstackptr = MCstaticdefaultstackptr = t_stack;
	
	t_stack -> extraopen(false);
	
	// Resolve parent scripts *after* we've loaded aux stacks.
	if (t_stack -> getextendedstate(ECS_USES_PARENTSCRIPTS))
		t_stack -> resolveparentscripts();
	
	MCscreen->resetcursors();
	MCImage::init();
	
#ifdef TARGET_SUBPLATFORM_ANDROID
	MCdispatcher -> loadexternal(MCSTR("revzip"));
	MCdispatcher -> loadexternal(MCSTR("revdb"));
	MCdispatcher -> loadexternal(MCSTR("revxml"));
	MCdispatcher -> loadexternal(MCSTR("dbsqlite"));
	MCdispatcher -> loadexternal(MCSTR("dbmysql"));
#else
	MCdispatcher -> loadexternal(MCSTR("revzip.dylib"));
	MCdispatcher -> loadexternal(MCSTR("revdb.dylib"));
#endif
	
	// MW-2010-12-18: Startup message / stack init now down in 'main'
#endif

	return IO_NORMAL;
}

#else

IO_stat MCDispatch::startup(void)
{
	startdir = MCS_getcurdir();
	enginedir = strdup(MCStringGetCString(MCcmd));
	char *eptr = strrchr(enginedir, PATH_SEPARATOR);
	if (eptr != NULL)
		*eptr = '\0';
	else
		*enginedir = '\0';
	char *openpath = strdup(MCStringGetCString(MCcmd)); //point to MCcmd string

#ifdef _DEBUG
#ifdef _WINDOWS
	// This little snippet of code allows an easy way to attach VS to a standalone
	// instance to debug startup.
	/*while(!IsDebuggerPresent())
		Sleep(50);
	Sleep(250);
	DebugBreak();*/
#endif

	if (MCcapsule . size == 0)
	{
		MCStack *t_stack;
		IO_handle t_stream;
		MCAutoStringRef t_env;
		/* UNCHECKED */ MCS_getenv(MCSTR("TEST_STACK"), &t_env);
		t_stream = MCS_open(*t_env, kMCSystemFileModeRead, False, False, 0);
		if (MCdispatcher -> readstartupstack(t_stream, t_stack) != IO_NORMAL)
		{
			MCresult -> sets("failed to read standalone stack");
			return IO_ERROR;
		}
		MCS_close(t_stream);
		
		/* UNCHECKED */ MCStringCreateWithCString(openpath, MCcmd);
		MCdefaultstackptr = MCstaticdefaultstackptr = t_stack;
		
		t_stack -> extraopen(false);
		
		MCModeResetCursors();
		MCImage::init();
		send_startup_message();
		if (!MCquit)
			t_stack -> open();

		return IO_NORMAL;
	}
#endif

	// The info structure that will be filled in while parsing the capsule.
	MCStandaloneCapsuleInfo t_info;
	memset(&t_info, 0, sizeof(MCStandaloneCapsuleInfo));

	// Create a capsule and fill with the standalone data
	MCCapsuleRef t_capsule;
	t_capsule = nil;
	if (!MCCapsuleOpen(MCStandaloneCapsuleCallback, &t_info, t_capsule))
		return IO_ERROR;

	if (((MCcapsule . size) & (1U << 31)) == 0)
	{
		// Capsule is not spilled - just use the project section.
		// MW-2010-05-08: Capsule size includes 'size' field, so need to adjust
		if (!MCCapsuleFillNoCopy(t_capsule, (const void *)&MCcapsule . data, MCcapsule . size - sizeof(uint32_t), true))
		{
			MCCapsuleClose(t_capsule);
			return IO_ERROR;
		}
	}
	else
	{
		// Capsule is spilled fill from:
		//   0..2044 from project section
		//   spill file
		//   rest from project section
		char *t_spill;
		t_spill = (char *)malloc(strlen(openpath) + 5);
		sprintf(t_spill, "%s.dat", openpath);
		if (!MCCapsuleFillFromFile(t_capsule, t_spill, 0, true))
		{
			free(t_spill);
			MCCapsuleClose(t_capsule);
			return IO_ERROR;
		}
		free(t_spill);
	}

	// Process the capsule
	if (!MCCapsuleProcess(t_capsule))
	{
		MCCapsuleClose(t_capsule);
		return IO_ERROR;
	}

	/* UNCHECKED */ MCStringCreateWithCString(openpath, MCcmd);
	MCdefaultstackptr = MCstaticdefaultstackptr = t_info . stack;
	MCCapsuleClose(t_capsule);

	// Initialization required.
	MCModeResetCursors();
	MCImage::init();

	// MW-2010-10-22: [[ Bug 8352 ]] Make sure allowInterrupts is off by default.
	MCallowinterrupts = False;

	// Now open the main stack.
	t_info . stack -> extraopen(false);
	send_startup_message();
	if (!MCquit)
		t_info . stack -> open();

	return IO_NORMAL;
}
#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of MCStack::mode* hooks for STANDALONE mode.
//

void MCStack::mode_create(void)
{
}

void MCStack::mode_copy(const MCStack& stack)
{
}

void MCStack::mode_destroy(void)
{
}

Exec_stat MCStack::mode_getprop(uint4 parid, Properties which, MCExecPoint &ep, MCStringRef carray, Boolean effective)
{
	return ES_NOT_HANDLED;
}

Exec_stat MCStack::mode_setprop(uint4 parid, Properties which, MCExecPoint &ep, MCStringRef cprop, MCStringRef carray, Boolean effective)
{
	return ES_NOT_HANDLED;
}

void MCStack::mode_load(void)
{
}

void MCStack::mode_getrealrect(MCRectangle& r_rect)
{
	MCscreen->getwindowgeometry(window, r_rect);
}

void MCStack::mode_takewindow(MCStack *other)
{
}

void MCStack::mode_takefocus(void)
{
	MCscreen->setinputfocus(window);
}

bool MCStack::mode_needstoopen(void)
{
	return true;
}

void MCStack::mode_setgeom(void)
{
}

void MCStack::mode_setcursor(void)
{
	MCscreen->setcursor(window, cursor);
}

bool MCStack::mode_openasdialog(void)
{
	return true;
}

void MCStack::mode_closeasdialog(void)
{
}

void MCStack::mode_openasmenu(MCStack *grab)
{
}

void MCStack::mode_closeasmenu(void)
{
}

bool MCStack::mode_haswindow(void)
{
	return window != DNULL;
}

void MCStack::mode_constrain(MCRectangle& rect)
{
}

#ifdef _WINDOWS
MCSysWindowHandle MCStack::getrealwindow(void)
{
	return window->handle.window;
}

MCSysWindowHandle MCStack::getqtwindow(void)
{
	return window->handle.window;
}
#endif

#ifdef _MACOSX
MCSysWindowHandle MCStack::getqtwindow(void)
{
	return window -> handle . window;
}
#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of MCObject::mode_get/setprop for STANDALONE mode.
//

Exec_stat MCObject::mode_getprop(uint4 parid, Properties which, MCExecPoint &ep, MCStringRef carray, Boolean effective)
{
	return ES_NOT_HANDLED;
}

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of MCProperty::mode_eval/mode_set for STANDALONE mode.
//

Exec_stat MCProperty::mode_set(MCExecPoint& ep)
{
	return ES_NOT_HANDLED;
}

Exec_stat MCProperty::mode_eval(MCExecPoint& ep)
{
	return ES_NOT_HANDLED;
}

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of mode hooks for STANDALONE mode.
//

// In standalone mode, the standalone stack built into the engine cannot
// be saved.
IO_stat MCModeCheckSaveStack(MCStack *sptr, const MCStringRef p_filename)
{
	if (sptr == MCdispatcher -> getstacks())
	{
		MCresult->sets("can't save into standalone");
		return IO_ERROR;
	}

	return IO_NORMAL;
}

// In standalone mode, the environment depends on various command-line/runtime
// globals.
MCNameRef MCModeGetEnvironment(void)
{
#ifdef _MOBILE
	return MCN_mobile;
#else
	// MW-2011-09-19: [[ Bug 9734 ]] If in '-ui' mode, return 'command line'.
	if (MCnoui)
		return MCN_command_line;

	if (MCnofiles)
		return MCN_helper_application;

	return MCN_standalone_application;
#endif
}

uint32_t MCModeGetEnvironmentType(void)
{
	if (MCnofiles)
		return kMCModeEnvironmentTypeHelper;
	return kMCModeEnvironmentTypeDesktop;
}

// In standalone mode, we are never licensed.
bool MCModeGetLicensed(void)
{
	return false;
}

// In standalone mode, the executable is $0 if there is an embedded stack.
bool MCModeIsExecutableFirstArgument(void)
{
	return true;
}

// In standalone mode, we only automatically open stacks if there isn't an
// embedded stack.
bool MCModeShouldLoadStacksOnStartup(void)
{
	return false;
}

// In standalone mode, we try to work out what went wrong...
void MCModeGetStartupErrorMessage(MCStringRef& r_caption, MCStringRef& r_text)
{
	r_caption = MCSTR("Initialization Error");
	if (MCValueGetTypeCode(MCresult -> getvalueref()) == kMCValueTypeCodeString)
		r_text = MCValueRetain((MCStringRef)MCresult -> getvalueref());
	else
		r_text = MCSTR("unknown reason");
}

// In standalone mode, we can only set an object's script if has non-zero id.
bool MCModeCanSetObjectScript(uint4 obj_id)
{
	return obj_id != 0;
}

// In standalone mode, we must check the old CANT_STANDALONE flag.
bool MCModeShouldCheckCantStandalone(void)
{
	return true;
}

// The standalone mode doesn't have a message box redirect feature
bool MCModeHandleMessageBoxChanged(MCExecContext& ctxt, MCStringRef p_msg)
{
	return false;
}

// The standalone mode causes a relaunch message.
bool MCModeHandleRelaunch(MCStringRef &r_id)
{
#ifdef _WINDOWS
	bool t_do_relaunch;
	t_do_relaunch = MCdefaultstackptr -> hashandler(HT_MESSAGE, MCM_relaunch) == True;
	/* UNCHECKED */ MCStringCopy(MCNameGetString(MCdefaultstackptr -> getname()), r_id);
	return t_do_relaunch;
#else
	return false;
#endif
}

// The standalone mode's startup stack depends on whether it has a stack embedded.
const char *MCModeGetStartupStack(void)
{
	return NULL;
}

bool MCModeCanLoadHome(void)
{
	return false;
}

MCStatement *MCModeNewCommand(int2 which)
{
	return NULL;
}

MCExpression *MCModeNewFunction(int2 which)
{
	return NULL;
}

void MCModeObjectDestroyed(MCObject *object)
{
}

MCObject *MCModeGetU3MessageTarget(void)
{
	return MCdefaultstackptr -> getcard();
}

bool MCModeShouldQueueOpeningStacks(void)
{
	return MCscreen == NULL;
}

bool MCModeShouldPreprocessOpeningStacks(void)
{
	return false;
}

Window MCModeGetParentWindow(void)
{
	Window t_window;
	t_window = MCdefaultstackptr -> getwindow();
	if (t_window == DNULL && MCtopstackptr != NULL)
		t_window = MCtopstackptr -> getwindow();
	return t_window;
}

bool MCModeCanAccessDomain(MCStringRef p_name)
{
	return false;
}

void MCModeQueueEvents(void)
{
}

Exec_stat MCModeExecuteScriptInBrowser(MCStringRef p_script)
{
	MCeerror -> add(EE_ENVDO_NOTSUPPORTED, 0, 0);
	return ES_ERROR;
}

bool MCModeMakeLocalWindows(void)
{
	return true;
}

void MCModeActivateIme(MCStack *p_stack, bool p_activate)
{
	MCscreen -> activateIME(p_activate);
}

void MCModeConfigureIme(MCStack *p_stack, bool p_enabled, int32_t x, int32_t y)
{
	if (!p_enabled)
		MCscreen -> clearIME(p_stack -> getwindow());
}

void MCModeShowToolTip(int32_t x, int32_t y, uint32_t text_size, uint32_t bg_color, MCStringRef text_font, MCStringRef message)
{
}

void MCModeHideToolTip(void)
{
}

void MCModeResetCursors(void)
{
	MCscreen -> resetcursors();
}

bool MCModeCollectEntropy(void)
{
	return true;
}

// We return false here as at present, in standalones, the first stack does not
// sit in the message path of all stacks.
bool MCModeHasHomeStack(void)
{
	return false;
}

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of remote dialog methods
//

void MCRemoteFileDialog(MCStringRef p_title, MCStringRef p_prompt, MCStringRef *p_types, uint32_t p_type_count, MCStringRef p_initial_folder, MCStringRef p_initial_file, bool p_save, bool p_files, MCStringRef &r_value)
{
}

void MCRemoteColorDialog(MCStringRef p_title, uint32_t p_r, uint32_t p_g, uint32_t p_b, bool &r_chosen, MCColor &r_chosen_color)
{
}

void MCRemoteFolderDialog(MCStringRef p_title, MCStringRef p_prompt, MCStringRef p_initial, MCStringRef &r_value)
{
}

void MCRemotePrintSetupDialog(MCDataRef p_config_data, MCDataRef &r_reply_data, uint32_t &r_result)
{
}

void MCRemotePageSetupDialog(MCDataRef p_config_data, MCDataRef &r_reply_data, uint32_t &r_result)
{
}

#ifdef _MACOSX
uint32_t MCModePopUpMenu(MCMacSysMenuHandle p_menu, int32_t p_x, int32_t p_y, uint32_t p_index, MCStack *p_stack)
{
	return 0;
}
#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of Windows-specific mode hooks for STANDALONE mode.
//

#ifdef TARGET_PLATFORM_WINDOWS

void MCModePreMain(void)
{
}

void MCModeSetupCrashReporting(void)
{
}

bool MCModeHandleMessage(LPARAM lparam)
{
	return false;
}

bool MCPlayer::mode_avi_closewindowonplaystop()
{
	return true;
}

#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of Mac OS X-specific mode hooks for DEVELOPMENT mode.
//

#ifdef _MACOSX

bool MCModePreWaitNextEvent(Boolean anyevent)
{
	return false;
}

#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Implementation of Linux-specific mode hooks for DEVELOPMENT mode.
//

#ifdef _LINUX

void MCModePreSelectHook(int& maxfd, fd_set& rfds, fd_set& wfds, fd_set& efds)
{
}

void MCModePostSelectHook(fd_set& rfds, fd_set& wfds, fd_set& efds)
{
}

#endif
