/**
 * \file
 */

#ifndef __MONO_DEBUGGER_AGENT_H__
#define __MONO_DEBUGGER_AGENT_H__

#include "mini.h"
#include <mono/utils/mono-stack-unwinding.h>

#define MONO_DBG_CALLBACKS_VERSION (4)
// 2. debug_log parameters changed from MonoString* to MonoStringHandle
// 3. debug_log parameters changed from MonoStringHandle back to MonoString*
// 4. is_enabled added

struct _MonoDebuggerCallbacks {
	int version;
	/* Says whether the agent is on. The runtime asks before it does work only the agent reads. */
	gboolean (*is_enabled) (void);
	void (*parse_options) (char *options);
	void (*init) (void);
	void (*breakpoint_hit) (void *sigctx);
	void (*single_step_event) (void *sigctx);
	void (*single_step_from_context) (MonoContext *ctx);
	void (*breakpoint_from_context) (MonoContext *ctx);
	void (*free_domain_info) (MonoDomain *domain);
	void (*unhandled_exception) (MonoException *exc);
	void (*handle_exception) (MonoException *exc, MonoContext *throw_ctx,
							  MonoContext *catch_ctx, StackFrameInfo *catch_frame);
	void (*begin_exception_filter) (MonoException *exc, MonoContext *ctx, MonoContext *orig_ctx);
	void (*end_exception_filter) (MonoException *exc, MonoContext *ctx, MonoContext *orig_ctx);
	void (*user_break) (void);
	void (*debug_log) (int level, MonoString *category, MonoString *message);
	gboolean (*debug_log_is_enabled) (void);
	void (*send_crash) (char *json_dump, MonoStackHash *hashes, int pause);
};

typedef struct _DebuggerTlsData DebuggerTlsData;

MONO_API void
mono_debugger_agent_init (void);

MONO_API void
mono_debugger_agent_parse_options (char *options);

void
mono_debugger_agent_stub_init (void);

MONO_API gboolean
mono_debugger_agent_transport_handshake (void);

typedef void (*MonoDebuggerAttachFunc) (gboolean attached);

MONO_API void
mono_debugger_set_generate_debug_info (gboolean enable);

MONO_API gboolean
mono_debugger_get_generate_debug_info (void);

MONO_API void
mono_debugger_disconnect (void);

MONO_API void
mono_debugger_install_attach_detach_callback (MonoDebuggerAttachFunc func);

#endif
