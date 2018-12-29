/*
 * Copyright (C) 2012 John Crispin <blogic@openwrt.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*** Binding for libubox into lua
 * @module uloop
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "../uloop.h"
#include "../list.h"

/*** A few notes about the callbacks.
 * When the raw uloop callback is called, the lua `state` is _always_ the main
 * coroutine of lua, and all callback execute on it.  The _cb functions _must_
 * start and end with the stack unmodified, or else very bad data corruption
 * will happen in lua.  In other words, _cb must leave the stack exactly as it
 * found it, if there were 0 items, it must leave 0 items, if there were 10 items
 * it must leave those exact same 10 items.
 *
 * When a lua callback function is passed in, it's wraped in a C closure,
 * using the Lua callback as the upvalue. We need a unique object to
 * create a reference from, and we can't assume the function passed in is
 * unique.  Why make that requirement on the developer? For example
 * `uloop.timer(uloop.cancel)` should be a valid call, and it should also not
 * conflict with any other functions that uloop creates, or anyone else uses
 * for a reference.  So create a new lua object, in this case a new closure.
 * It was a single up value, the callback is should call.  This then guarantees
 * that the call to luaL_ref returns a unique value.
 */

#define MODNAME		"uloop"
#define TIMER_METANAME		MODNAME ".timer"
#define PROCESS_METANAME	MODNAME ".process"
#define FD_METANAME			MODNAME ".fd"

struct lua_uloop_fd {
	struct uloop_fd fd;
	int r;
	int fd_r;
};

struct lua_uloop_timeout {
	struct uloop_timeout t;
	int r;
};

struct lua_uloop_process {
	struct uloop_process p;
	int r;
};

static lua_State *state;

static __attribute__((unused)) void stackDump (lua_State *L) {
	int i;
	int top = lua_gettop(L);  /* depth of the stack */
	for (i = 1; i <= top; i++) {  /* repeat for each level */
		int t = lua_type(L, i);
		switch (t) {
			case LUA_TSTRING: {  /* strings */
								  printf("%d: ", i);
								  printf("'%s'", lua_tostring(L, i));
								  break;
							  }
			case LUA_TBOOLEAN: {  /* Booleans */
								  printf("%d: ", i);
								   printf(lua_toboolean(L, i) ? "true" : "false");
								   break; }
			case LUA_TNUMBER: {  /* numbers */
								  printf("%d: ", i);
								  printf("%g", lua_tonumber(L, i));
								  break;
							  }
			default: {  /* other values */
						 lua_getglobal(L, "tostring");
						 lua_pushvalue(L, i);
						 lua_call(L, 1, 1);
						 const char *tostring = lua_tostring(L, -1);
						 lua_pop(L, 1);
						 printf("%d: %s", i, tostring);
						 break; }
		}
		printf(", ");  /* put a separator */
	}
	printf("\n");  /* end the listing */
}

static void ul_timer_cb(struct uloop_timeout *t)
{
	struct lua_uloop_timeout *tout = container_of(t, struct lua_uloop_timeout, t);

	lua_rawgeti(state, LUA_REGISTRYINDEX, tout->r);

	lua_call(state, 0, 0);
}

/*
 * Stored with the upvalues:
 * 1. lua callback
 * And called with the no arguments
 */
static int ul_timer_closure(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_call(state, 0, 0);
	return 0;
}

static int ul_timer_set(lua_State *L)
{
	struct lua_uloop_timeout *tout = luaL_checkudata(L, 1, TIMER_METANAME);
	lua_Integer set;
	int isint;

	luaL_checkinteger(L, -1);

	set = lua_tointegerx(L, -1, &isint);
	if (!isint) {
		lua_Number n = lua_tonumber(L, -1);
		set = n;
	}
	uloop_timeout_set(&tout->t, set);

	return 1;
}

static int ul_timer_remaining(lua_State *L)
{
	struct lua_uloop_timeout *tout = luaL_checkudata(L, 1, TIMER_METANAME);

	tout = lua_touserdata(L, 1);
	lua_pushnumber(L, uloop_timeout_remaining(&tout->t));
	return 1;
}

static int ul_timer_cancel(lua_State *L)
{
	struct lua_uloop_timeout *tout = luaL_checkudata(L, 1, TIMER_METANAME);

	uloop_timeout_cancel(&tout->t);

	return 0;
}

static int ul_timer__gc(lua_State *L)
{
	struct lua_uloop_timeout *tout = luaL_checkudata(L, 1, TIMER_METANAME);

	uloop_timeout_cancel(&tout->t);

	luaL_unref(state, LUA_REGISTRYINDEX, tout->r);

	return 1;
}

static const luaL_Reg timer_m[] = {
	{ "set", ul_timer_set },
	{ "remaining", ul_timer_remaining },
	{ "cancel", ul_timer_cancel },
	{ "__gc", ul_timer__gc },
	{ NULL, NULL }
};

/*** Create a new timer.
 * tparam[opt] integer Number of milliseconds to set the timer for
 * tparam func Callback to call when the timer expired
 * treturn uloop.timer A new timer object
 */
static int ul_timer(lua_State *L)
{
	struct lua_uloop_timeout *tout;
	lua_Integer set = 0;
	int is_set;
	int ref;

	luaL_checktype(L, 1, LUA_TFUNCTION);

	set = lua_tointegerx(L, -1, &is_set);
	if (is_set) {
		lua_pop(L, 1);
	}

	lua_pushcclosure(L, ul_timer_closure, 1);

	ref = luaL_ref(L, LUA_REGISTRYINDEX);

	tout = lua_newuserdata(L, sizeof(struct lua_uloop_timeout));
	luaL_setmetatable(L, TIMER_METANAME);
	memset(tout, 0, sizeof(struct lua_uloop_timeout));
	tout->t.cb = ul_timer_cb;

	tout->r = ref;

	if (is_set)
		uloop_timeout_set(&tout->t, set);

	return 1;
}

static void ul_ufd_cb(struct uloop_fd *fd, unsigned int events)
{
	struct lua_uloop_fd *ufd = container_of(fd, struct lua_uloop_fd, fd);

	lua_rawgeti(state, LUA_REGISTRYINDEX, ufd->r);

	/* push events */
	lua_pushinteger(state, events);
	lua_call(state, 1, 0);
}

/*
 * Stored with the upvalues:
 * 1. fd
 * 2. lua callback
 * And called with the arguments
 * 1. Events
 */
static int ul_ufd_closure(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(2));
	lua_pushvalue(L, lua_upvalueindex(1));
	/* stack is now: 1. events 2. lua call back 3. fd */
	lua_pushvalue(L, 1);
	/* stack is now: 1. events 2. lua callback 3. fd 4. events */

	/* Should be called with:
	 * 1. fd
	 * 2. events
	 */
	lua_call(state, 2, 0);
	return 0;
}

static int get_sock_fd(lua_State* L, int idx) {
	int isint = 0;
	int fd;
	fd = lua_tointegerx(L, idx, &isint);
	if (!isint) {
		luaL_checktype(L, idx, LUA_TUSERDATA);
		lua_getfield(L, idx, "getfd");
		if(lua_isnil(L, -1))
			return luaL_error(L, "socket type missing 'getfd' method");
		lua_pushvalue(L, idx - 1);
		lua_call(L, 1, 1);
		fd = luaL_checkinteger(L, -1);
		lua_pop(L, 1);
	}
	return fd;
}

static int ul_ufd_delete(lua_State *L)
{
	struct lua_uloop_fd *ufd = luaL_checkudata(L, 1, FD_METANAME);

	uloop_fd_delete(&ufd->fd);
	return 0;
}

static int ul_ufd__gc(lua_State *L)
{
	struct lua_uloop_fd *ufd = luaL_checkudata(L, 1, FD_METANAME);

	uloop_fd_delete(&ufd->fd);
	luaL_unref(L, LUA_REGISTRYINDEX, ufd->r);

	return 0;
}

static const luaL_Reg ufd_m[] = {
	{ "delete", ul_ufd_delete },
	{ "__gc", ul_ufd__gc },
	{ NULL, NULL }
};

/***
 * Add a file descriptor to uloop for call on activity.
 * @function fd_add
 * @tparam int fd File Descriptor to add.
 * @tparam function func Callback function.  
 *                       The callback is called with the following parameters
 *                       1. fd
 *                       2. event
 * @tparam int Option flags.
 *             Must be a combinatio of: uloop.ULOOP_READ, uloop.ULOOP_WRITE,
 *             uloop.EDGE_TRIGGER, uloop.ULOOP_BLOCKING
 */
static int ul_ufd_add(lua_State *L)
{
	struct lua_uloop_fd *ufd;
	int fd = 0;
	lua_Integer flags;
	int ref;

	fd = get_sock_fd(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	flags = luaL_checkinteger(L, 3);
	lua_pop(L, 1);

	/*
	* Upvalues:
	* 1. fd
	* 2. Lua callback
	*/
	lua_pushcclosure(L, ul_ufd_closure, 2);

	ref = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_pop(L, 1);

	ufd = lua_newuserdata(L, sizeof(struct lua_uloop_fd));
	luaL_setmetatable(L, FD_METANAME);

	memset(ufd, 0, sizeof(struct lua_uloop_fd));
	ufd->r = ref;
	ufd->fd.fd = fd;
	ufd->fd.cb = ul_ufd_cb;

	uloop_fd_add(&ufd->fd, flags);

	return 1;
}

static int ul_process_delete(lua_State *L)
{
	struct lua_uloop_process *proc = luaL_checkudata(L, 1, PROCESS_METANAME);

	uloop_process_delete(&proc->p);

	return 0;
}

static int ul_process__gc(lua_State *L)
{
	struct lua_uloop_process *proc = luaL_checkudata(L, 1, PROCESS_METANAME);

	uloop_process_delete(&proc->p);
	luaL_unref(state, LUA_REGISTRYINDEX, proc->r);

	return 0;
}

static int ul_process_pid(lua_State *L)
{
	struct lua_uloop_process *proc = luaL_checkudata(L, 1, PROCESS_METANAME);

	if (proc->p.pid) {
		lua_pushnumber(L, proc->p.pid);
		return 1;
	}

	return 0;
}

static const luaL_Reg process_m[] = {
	{ "delete", ul_process_delete },
	{ "pid", ul_process_pid },
	{ "__gc", ul_process__gc },
	{ NULL, NULL }
};

static void ul_process_cb(struct uloop_process *p, int ret)
{
	struct lua_uloop_process *proc = container_of(p, struct lua_uloop_process, p);

	lua_rawgeti(state, LUA_REGISTRYINDEX, proc->r);

	lua_pushinteger(state, ret >> 8);
	lua_call(state, 1, 0);
}

/*
 * Stored with the upvalues:
 * 2. lua callback
 * And called with the arguments
 * 1. ret/signaled status
 */
static int ul_process_closure(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	/* push events */
	lua_pushvalue(L, 1);

	lua_call(state, 1, 0);
	return 0;
}

/***
 * Create a new process and get notified of its termination
 * @function process
 * @tparam string process to execute
 * @tparam table A table of arguments to pass to the process
 * @tparam table A table of environment variables to set
 * @tparam function Callback when the process exits
 */
static int ul_process(lua_State *L)
{
	struct lua_uloop_process *proc;
	pid_t pid;
	int ref;

	luaL_checktype(L, 1, LUA_TSTRING);
	luaL_checktype(L, 2, LUA_TTABLE);
	luaL_checktype(L, 3, LUA_TTABLE);
	luaL_checktype(L, 4, LUA_TFUNCTION);

	pid = fork();

	if (pid == -1) {
		return luaL_error(L, "failed to fork");
	}

	if (pid == 0) {
		/* child */
		int argn = lua_rawlen(L, 2);
		int envn = lua_rawlen(L, 3);
		char** argp = malloc(sizeof(char*) * (argn + 2));
		char** envp = malloc(sizeof(char*) * (envn + 1));
		int i = 1;

		if (!argp || !envp)
			_exit(-1);

		argp[0] = (char*) lua_tostring(L, 1);
		for (i = 1; i <= argn; i++) {
			lua_rawgeti(L, -3, i);
			argp[i] = (char*) lua_tostring(L, -1);
			lua_pop(L, 1);
		}
		argp[i] = NULL;

		for (i = 1; i <= envn; i++) {
			lua_rawgeti(L, -2, i);
			envp[i - 1] = (char*) lua_tostring(L, -1);
			lua_pop(L, 1);
		}
		envp[i - 1] = NULL;

		execve(*argp, argp, envp);
		_exit(-1);
	}

	lua_pushcclosure(L, ul_process_closure, 1);
	ref = luaL_ref(L, LUA_REGISTRYINDEX);

	proc = lua_newuserdata(L, sizeof(struct lua_uloop_process));
	luaL_setmetatable(L, PROCESS_METANAME);

	memset(proc, 0, sizeof(struct lua_uloop_process));
	proc->r = ref;
	proc->p.pid = pid;
	proc->p.cb = ul_process_cb;
	uloop_process_add(&proc->p);

	return 1;
}

/***
 * Create a new process and get notified of its termination
 * @function pid_add
 * @tparam int pid The process id
 * @tparam function Callback when the process exits
 */
static int ul_pid_add(lua_State *L)
{
	struct lua_uloop_process *proc;
	lua_Integer pid;
	int ref;

	pid = luaL_checkinteger(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	lua_pushcclosure(L, ul_process_closure, 1);
	ref = luaL_ref(L, LUA_REGISTRYINDEX);

	proc = lua_newuserdata(L, sizeof(struct lua_uloop_process));
	luaL_setmetatable(L, PROCESS_METANAME);

	memset(proc, 0, sizeof(struct lua_uloop_process));
	proc->r = ref;
	proc->p.pid = pid;
	proc->p.cb = ul_process_cb;
	uloop_process_add(&proc->p);

	return 1;
}

static int ul_init(lua_State *L)
{
	uloop_init();
	lua_pushboolean(L, 1);

	return 1;
}

static int ul_run(lua_State *L)
{
	uloop_run();
	lua_pushboolean(L, 1);

	return 1;
}

static int ul_end(lua_State *L)
{
	uloop_end();
	return 1;
}

static luaL_Reg uloop_func[] = {
	{"init", ul_init},
	{"run", ul_run},
	{"timer", ul_timer},
	{"process", ul_process},
	{"pid_add", ul_pid_add},
	{"fd_add", ul_ufd_add},
	{"cancel", ul_end},
	{NULL, NULL},
};

/* avoid warnings about missing declarations */
int luaopen_uloop(lua_State *L);
int luaclose_uloop(lua_State *L);

static void ul_create_metatable(lua_State *L, const luaL_Reg *funcs, const char *name)
{
	/* create metatable for the timers */
	luaL_newmetatable(L, name);

	/* metatable.__index = metatable */
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	/* fill metatable */
	luaL_setfuncs(L, funcs, 0);
	lua_pop(L, 1);
}

int luaopen_uloop(lua_State *L)
{
	state = L;

	ul_create_metatable(L, timer_m, TIMER_METANAME);
	ul_create_metatable(L, ufd_m, FD_METANAME);
	ul_create_metatable(L, process_m, PROCESS_METANAME);

	luaL_newlib(L, uloop_func);
	lua_pushstring(L, "_VERSION");
	lua_pushstring(L, "1.3");
	lua_rawset(L, -3);

	lua_pushstring(L, "ULOOP_READ");
	lua_pushinteger(L, ULOOP_READ);
	lua_rawset(L, -3);

	lua_pushstring(L, "ULOOP_WRITE");
	lua_pushinteger(L, ULOOP_WRITE);
	lua_rawset(L, -3);

	lua_pushstring(L, "ULOOP_EDGE_TRIGGER");
	lua_pushinteger(L, ULOOP_EDGE_TRIGGER);
	lua_rawset(L, -3);

	lua_pushstring(L, "ULOOP_BLOCKING");
	lua_pushinteger(L, ULOOP_BLOCKING);
	lua_rawset(L, -3);

	return 1;
}

int luaclose_uloop(lua_State *L)
{
	lua_pushstring(L, "Called");

	return 1;
}
