#include <assert.h>
#include <security/pam_appl.h>
#include <security/pam_modules.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __APPLE__
#include <security/pam_ext.h>
#endif

#include <lauxlib.h>
#include <lua.h>

#include "lextlib/lextlib.h"

#define L_PAM_HANDLE_T "pam_handle_t"

static int lua_conversation(int num_msg, const struct pam_message **msg,
                            struct pam_response **resp, void *appdata_ptr) {
  lua_State *L = appdata_ptr;

  int base = lua_gettop(L);

  lua_createtable(L, num_msg, 0);
  int table = lua_gettop(L);

  for (int i = 0; i < num_msg; i++) {
    lua_createtable(L, 2, 0);
    int message = lua_gettop(L);

    lua_pushinteger(L, msg[i]->msg_style);
    lua_rawseti(L, message, 1);

    lua_pushstring(L, msg[i]->msg);
    lua_rawseti(L, message, 2);

    lua_rawseti(L, table, i + 1);
  }

  lua_pushlightuserdata(L, appdata_ptr);
  lua_gettable(L, LUA_REGISTRYINDEX);
  int appdata = lua_gettop(L);

  /* Conversation function */
  lua_rawgeti(L, appdata, 1);
  /* PAM messages */
  lua_pushvalue(L, table);
  /* Lua appdata */
  lua_rawgeti(L, appdata, 2);

  int lerr = lua_pcall(L, 2, 2, 0);
  if (lerr != LUA_OK) {
    lua_insert(L, base + 1);
    lua_settop(L, base + 1);

    return PAM_CONV_ERR;
  }

  int responses = lua_gettop(L) - 1;
  if (lua_isnil(L, responses)) {
    if (!lua_isnil(L, responses + 1)) {
      lua_insert(L, base + 1);
      lua_settop(L, base + 1);
    } else {
      lua_settop(L, base);
      lua_pushstring(L, "Unknown error");
    }

    return PAM_CONV_ERR;
  }
  luaX_checktype(L, responses, "responses", LUA_TTABLE);

  int num_res = 0;
  *resp = malloc(num_msg * sizeof(struct pam_response));
  lua_pushnil(L);
  for (int i = 0; i < num_msg; i++) {
    if (lua_next(L, responses) == 0) {
      lua_settop(L, base);
      lua_pushfstring(
          L, "Number of responses (%d) does not match number of messages (%d)",
          num_res, num_msg);

      return PAM_CONV_ERR;
    }
    int response = lua_gettop(L);

    luaX_checktype(L, response, "responses[i]", LUA_TTABLE);

    lua_rawgeti(L, response, 1);
    const char *_resp = luaX_checkstring(L, -1, "responses[i].resp");
    (*resp)[i].resp = strdup(_resp);

    lua_rawgeti(L, response, 2);
    (*resp)[i].resp_retcode =
        luaX_checkinteger(L, -1, "responses[i].resp_retcode");

    num_res++;

    lua_pop(L, 3);
  }

  lua_settop(L, base);

  return PAM_SUCCESS;
}

static int lua_pam_notimplemented(lua_State *L) {
  return luaL_error(L, "Not implemented");
}

static int lua_pam_start(lua_State *L) {
  const char *service_name = luaX_checkstring(L, 1, "service_name");
  const char *user = luaX_optstring(L, 2, "user", NULL);
  luaX_checktype(L, 3, "pam_conversation", LUA_TTABLE);

  lua_rawgeti(L, 3, 1);
  luaX_checktype(L, -1, "pam_conversation[1]", LUA_TFUNCTION);

  struct pam_conv *pam_conversation = malloc(sizeof(*pam_conversation));
  pam_conversation->conv = lua_conversation;
  pam_conversation->appdata_ptr = L;

  lua_pushlightuserdata(L, pam_conversation->appdata_ptr);
  lua_pushvalue(L, 3);
  lua_settable(L, LUA_REGISTRYINDEX);

  int lretval = lua_gettop(L) + 1;

  pam_handle_t *pamh = NULL;
  int err = pam_start(service_name, user, pam_conversation, &pamh);
  if (err != PAM_SUCCESS) {
    const char *errstr = pam_strerror(pamh, err);
    lua_pushnil(L);
    lua_pushstring(L, errstr);

    /* Conversation errors usually leave more detailed descriptions on the stack
     */
    int nresults = lua_gettop(L) - (lretval - 1) - 2;
    if (nresults != 0) {
      if (nresults != 1) {
        return luaL_error(
            L,
            "Expected 1 error message from conversation function, received %d",
            nresults);
      } else if (!lua_isstring(L, lretval)) {
        return luaL_error(
            L, "Error message from conversation function should be a string");
      }

      lua_pushstring(L, ": ");
      lua_pushvalue(L, lretval);
      lua_concat(L, 3);
    }

    return 2;
  }

  lua_pushlightuserdata(L, pamh);
  luaL_setmetatable(L, L_PAM_HANDLE_T);

  return 1;
}

static int lua_pam_end(lua_State *L) {
  pam_handle_t *pamh = luaX_checkudata(L, 1, "pamh", L_PAM_HANDLE_T);
  int pam_status = luaX_checknumber(L, 2, "pam_status");

  int err = pam_end(pamh, pam_status);
  if (err != PAM_SUCCESS) {
    const char *errstr = pam_strerror(pamh, err);
    lua_pushnil(L);
    lua_pushstring(L, errstr);
    return 2;
  }

  lua_pushboolean(L, true);

  return 1;
}

static int lua_pam_authenticate(lua_State *L) {
  pam_handle_t *pamh = luaX_checkudata(L, 1, "pamh", L_PAM_HANDLE_T);
  int flags = 0;
  if (!lua_isnoneornil(L, 2)) {
    luaX_checktype(L, 2, "flags", LUA_TTABLE);

    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
      flags |= lua_tointeger(
          L, -1); /* FIXME: I don't think this works with Lua... */
      lua_pop(L, 1);
    }
  }

  int lretval = lua_gettop(L) + 1;
  int err = pam_authenticate(pamh, flags);
  if (err != PAM_SUCCESS) {
    const char *errstr = pam_strerror(pamh, err);
    lua_pushnil(L);
    lua_pushstring(L, errstr);

    /* Conversation errors usually leave more detailed descriptions on the stack
     */
    int nresults = lua_gettop(L) - (lretval - 1) - 2;
    if (nresults != 0) {
      if (nresults != 1) {
        return luaL_error(
            L,
            "Expected 1 error message from conversation function, received %d",
            nresults);
      } else if (!lua_isstring(L, lretval)) {
        return luaL_error(
            L, "Error message from conversation function should be a string");
      }

      lua_pushstring(L, ": ");
      lua_pushvalue(L, lretval);
      lua_concat(L, 3);
    }

    return 2;
  }

  lua_pushboolean(L, true);

  return 1;
}

static int lua_pam_set_item(lua_State *L) {
  pam_handle_t *pamh = luaX_checkudata(L, 1, "pamh", L_PAM_HANDLE_T);
  int item_type = luaX_checkinteger(L, 2, "item_type");

  if (item_type == PAM_CONV) {
    /* Needs to code used for pam_conversation in pam_start */
    return lua_pam_notimplemented(L);
  }
#ifdef PAM_FAIL_DELAY
  else if (item_type == PAM_FAIL_DELAY) {
    /* Needs a wrapper for delay_fn */
    return lua_pam_notimplemented(L);
  }
#endif

  const char *item_s = luaX_checkstring(L, 3, "item");

  int err = pam_set_item(pamh, item_type, item_s);
  if (err != PAM_SUCCESS) {
    const char *errstr = pam_strerror(pamh, err);
    lua_pushnil(L);
    lua_pushstring(L, errstr);
    return 2;
  }

  lua_pushboolean(L, true);

  return 1;
}

static int lua_pam_strerror(lua_State *L) {
  pam_handle_t *pamh = luaX_checkudata(L, 1, "pamh", L_PAM_HANDLE_T);
  int errnum = luaX_checknumber(L, 2, "errnum");

  const char *err = pam_strerror(pamh, errnum);
  lua_pushstring(L, err);

  return 1;
}

static const luaL_Reg lua_pam_lib[] = {{"start", lua_pam_start},
                                       {"endx", lua_pam_end},

                                       {"authenticate", lua_pam_authenticate},
                                       {"setcred", NULL},

                                       {"acct_mgmt", NULL},

                                       {"chauthtok", NULL},

                                       {"open_session", NULL},

                                       {"set_item", lua_pam_set_item},
                                       {"get_item", NULL},
                                       {"get_user", NULL},
                                       {"set_data", NULL},
                                       {"get_data", NULL},

                                       {"putenv", NULL},
                                       {"getenv", NULL},
                                       {"getenvlist", NULL},

                                       {"strerror", lua_pam_strerror},

                                       {NULL, NULL}};

static int lua_pam_handle_t_tostring(lua_State *L) {
  pam_handle_t *pamh = lua_touserdata(L, 1);

  lua_pushfstring(L, "pam_handle_t: %p", pamh);

  return 1;
}

static const luaL_Reg lua_pam_handle_t_lib[] = {
    {"__tostring", lua_pam_handle_t_tostring},

    {NULL, NULL}};

int luaopen_pam(lua_State *L) {
  luaL_newmetatable(L, L_PAM_HANDLE_T);
  int metatable = lua_gettop(L);

  luaL_setfuncs(L, lua_pam_handle_t_lib, 0);

  luaL_newlib(L, lua_pam_lib);
  int pam = lua_gettop(L);

  lua_pushvalue(L, pam);
  lua_setfield(L, metatable, "__index");

  /* Error codes */
  luaX_setconst(L, pam, PAM_, ABORT);
  luaX_setconst(L, pam, PAM_, ACCT_EXPIRED);
  luaX_setconst(L, pam, PAM_, AUTHINFO_UNAVAIL);
  luaX_setconst(L, pam, PAM_, AUTHTOK_DISABLE_AGING);
  luaX_setconst(L, pam, PAM_, AUTHTOK_ERR);
  luaX_setconst(L, pam, PAM_, AUTHTOK_EXPIRED);
  luaX_setconst(L, pam, PAM_, AUTHTOK_LOCK_BUSY);
  luaX_setconst(L, pam, PAM_, AUTHTOK_RECOVERY_ERR);
  luaX_setconst(L, pam, PAM_, AUTH_ERR);
  luaX_setconst(L, pam, PAM_, BUF_ERR);
  luaX_setconst(L, pam, PAM_, CONV_ERR);
  luaX_setconst(L, pam, PAM_, CRED_ERR);
  luaX_setconst(L, pam, PAM_, CRED_EXPIRED);
  luaX_setconst(L, pam, PAM_, CRED_INSUFFICIENT);
  luaX_setconst(L, pam, PAM_, CRED_UNAVAIL);
  luaX_setconst(L, pam, PAM_, IGNORE);
  luaX_setconst(L, pam, PAM_, MAXTRIES);
  luaX_setconst(L, pam, PAM_, MODULE_UNKNOWN);
  luaX_setconst(L, pam, PAM_, NEW_AUTHTOK_REQD);
  luaX_setconst(L, pam, PAM_, NO_MODULE_DATA);
  luaX_setconst(L, pam, PAM_, OPEN_ERR);
  luaX_setconst(L, pam, PAM_, PERM_DENIED);
  luaX_setconst(L, pam, PAM_, SERVICE_ERR);
  luaX_setconst(L, pam, PAM_, SESSION_ERR);
  luaX_setconst(L, pam, PAM_, SUCCESS);
  luaX_setconst(L, pam, PAM_, SYMBOL_ERR);
  luaX_setconst(L, pam, PAM_, SYSTEM_ERR);
  luaX_setconst(L, pam, PAM_, TRY_AGAIN);
  luaX_setconst(L, pam, PAM_, USER_UNKNOWN);

  /* Conversation message types */
  luaX_setconst(L, pam, PAM_, PROMPT_ECHO_OFF);
  luaX_setconst(L, pam, PAM_, PROMPT_ECHO_ON);
  luaX_setconst(L, pam, PAM_, ERROR_MSG);
  luaX_setconst(L, pam, PAM_, TEXT_INFO);

  /* Item types */
  luaX_setconst(L, pam, PAM_, SERVICE);
  luaX_setconst(L, pam, PAM_, USER);
  luaX_setconst(L, pam, PAM_, USER_PROMPT);
  luaX_setconst(L, pam, PAM_, TTY);
  luaX_setconst(L, pam, PAM_, RUSER);
  luaX_setconst(L, pam, PAM_, RHOST);
  luaX_setconst(L, pam, PAM_, AUTHTOK);
  luaX_setconst(L, pam, PAM_, OLDAUTHTOK);
  luaX_setconst(L, pam, PAM_, CONV);
#ifdef PAM_FAIL_DELAY
  luaX_setconst(L, pam, PAM_, FAIL_DELAY);
#endif
#ifdef PAM_XDISPLAY
  luaX_setconst(L, pam, PAM_, XDISPLAY);
#endif
#ifdef PAM_XAUTHDATA
  luaX_setconst(L, pam, PAM_, XAUTHDATA);
#endif
#ifdef PAM_AUTHTOK_TYPE
  luaX_setconst(L, pam, PAM_, AUTHTOK_TYPE);
#endif

  return 1;
}
