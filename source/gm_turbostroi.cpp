#include "GarrysMod/Lua/Interface.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
using namespace GarrysMod::Lua;

#define SIMC_LIBRARY
extern "C" {
	#include "sim_core.h"
	#include "lua.h"
	#include "luajit.h"
	#include "lualib.h"
	#include "lauxlib.h"
}

//------------------------------------------------------------------------------
// Shared thread printing stuff
//------------------------------------------------------------------------------
#define BUFFER_SIZE 131072
#define QUEUE_SIZE 32768
double target_time = 0.0;
double rate = 100.0; //FPS
char metrostroiSystemsList[BUFFER_SIZE] = { 0 };
char loadSystemsList[BUFFER_SIZE] = { 0 };

typedef struct {
	int message;
	char system_name[64];
	char name[64];
	double index;
	double value;
} thread_msg;

typedef struct {
	double current_time;
	lua_State* L;
	int finished;

	SIMC_QUEUE* thread_to_sim;
	SIMC_QUEUE* sim_to_thread;
} thread_userdata;

char printMessageBuf[BUFFER_SIZE] = { 0 };
SIMC_LOCK_ID printMessageBufLock;
int shared_print(lua_State* L) {
	int n = lua_gettop(L);
	int i;
	char buffer[8192];
	char* buf = buffer;
	buffer[0] = 0;

	lua_getglobal(L, "tostring");
	for (i = 1; i <= n; i++) {
		const char* str;
		lua_pushvalue(L, -1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);
		str = lua_tostring(L, -1);
		if (strlen(str)+strlen(buffer) < 8192) {
			strcpy(buf,str);
			buf = buf + strlen(buf);
			buf[0] = '\t';
			buf = buf + 1;
			buf[0] = 0;
		}
		lua_pop(L, 1);
	}
	buffer[strlen(buffer)-1] = '\n';

	SIMC_Lock_Enter(printMessageBufLock);
		strncat(printMessageBuf,buffer,(BUFFER_SIZE-1) - strlen(buffer));
	SIMC_Lock_Leave(printMessageBufLock);
	return 0;
}

int thread_sendmessage(lua_State* state) {
	luaL_checktype(state,1,LUA_TNUMBER);
	luaL_checktype(state,2,LUA_TSTRING);
	luaL_checktype(state,3,LUA_TSTRING);
	luaL_checktype(state,4,LUA_TNUMBER);
	luaL_checktype(state,5,LUA_TNUMBER);

	lua_getglobal(state,"_userdata");
	thread_userdata* userdata = (thread_userdata*)lua_touserdata(state,-1);
	lua_pop(state,1);

	if (userdata) {
		thread_msg* tmsg;
		SIMC_Queue_EnterWrite(userdata->thread_to_sim,(void**)&tmsg);
			tmsg->message = (int)lua_tonumber(state,1);
			strncpy(tmsg->system_name,lua_tostring(state,2),63);
			tmsg->system_name[63] = 0;
			strncpy(tmsg->name,lua_tostring(state,3),63);
			tmsg->name[63] = 0;
			tmsg->index = lua_tonumber(state,4);
			tmsg->value = lua_tonumber(state,5);
		SIMC_Queue_LeaveWrite(userdata->thread_to_sim);
	}
	return 0;
}

int thread_recvmessage(lua_State* state) {
	lua_getglobal(state,"_userdata");
	thread_userdata* userdata = (thread_userdata*)lua_touserdata(state,-1);
	lua_pop(state,1);

	if (userdata) {
		thread_msg* tmsg;
		if (SIMC_Queue_EnterRead(userdata->sim_to_thread,(void**)&tmsg)) {
			lua_pushnumber(state,tmsg->message);
			lua_pushstring(state,tmsg->system_name);
			lua_pushstring(state,tmsg->name);
			lua_pushnumber(state,tmsg->index);
			lua_pushnumber(state,tmsg->value);
			SIMC_Queue_LeaveRead(userdata->sim_to_thread);
			return 5;
		}		
	}
	return 0;
}




//------------------------------------------------------------------------------
// Simulation thread
//------------------------------------------------------------------------------
void threadSimulation(thread_userdata* userdata) {
	lua_State* L = userdata->L;

	while (!userdata->finished) {
		lua_settop(L,0);

		//Avoid massive lag
		if (target_time - userdata->current_time > 1.0) {
			userdata->current_time = target_time;
		}

		//Simulate one step
		while (userdata->current_time < target_time) {
			lua_pushnumber(L,userdata->current_time);
			lua_setglobal(L,"CurrentTime");
			
			//Execute think
			lua_getglobal(L,"Think");
			if (lua_pcall(L,0,0,0)) {
				lua_pushcfunction(L,shared_print);
				lua_pushvalue(L,-2);
				lua_call(L,1,0);
				lua_pop(L,1);
			}
			userdata->current_time += 1.0/rate;
		}
		SIMC_Thread_Sleep(0.0);
	}

	//Release resources
	lua_pushcfunction(L,shared_print);
	lua_pushstring(L,"[!] Terminating thread");
	lua_call(L,1,0);
	lua_close(L);
	free(userdata);
}




//------------------------------------------------------------------------------
// Metrostroi Lua API
//------------------------------------------------------------------------------
void loadLua(lua_State* state, lua_State* L, char* filename) {
	//Load up "sv_turbostroi.lua" in the new JIT environment
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1,"file");
	LUA->GetField(-1,"Read");
	LUA->PushString(filename);
	LUA->PushString("LUA");
	LUA->Call(2,1); //file.Read(...)
	if (LUA->GetString(-1)) {
			
		if (luaL_loadbuffer(L, LUA->GetString(-1), strlen(LUA->GetString(-1)), filename) || lua_pcall(L, 0, LUA_MULTRET, 0)) {
			char buf[8192] = { 0 };
			LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
			LUA->GetField(-1,"MsgC");
			LUA->GetField(-2,"Color");
			LUA->PushNumber(255);
			LUA->PushNumber(0);
			LUA->PushNumber(127);
			LUA->Call(3,1);
			LUA->PushString(lua_tostring(L,-1));
			LUA->Call(2,0);
			LUA->Pop(); //GLOB
			lua_pop(L,1);
		}
	} else {
		LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
		LUA->GetField(-1,"MsgC");
		LUA->GetField(-2,"Color");
		LUA->PushNumber(255);
		LUA->PushNumber(0);
		LUA->PushNumber(127);
		LUA->Call(3,1);
		LUA->PushString("File not found!");
		LUA->Call(2,0);
		LUA->Pop(); //GLOB
	}
	LUA->Pop(); //returned result
	LUA->Pop(); //file
	LUA->Pop(); //GLOB
}

int API_InitializeTrain(lua_State* state) {
	//if (!LUA->CheckType(1, Type::USERDATA)) return 0;'

	//Add entry for train
	/*LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1,"Turbostroi");
	LUA->GetField(-1,"TrainData");
	LUA->Push(1);
	LUA->PushString("");
	LUA->SetTable(-3);
	LUA->Pop(); //TrainData
	LUA->Pop(); //Turbostroi
	LUA->Pop(); //GLOB*/

	//Initialize LuaJIT for train
	lua_State* L  = luaL_newstate();
	luaL_openlibs(L);
	luaopen_bit(L);
	lua_pushboolean(L,1);
	lua_setglobal(L,"TURBOSTROI");
	lua_pushcfunction(L,shared_print);
	lua_setglobal(L,"print");
	lua_pushcfunction(L,thread_sendmessage);
	lua_setglobal(L,"SendMessage");
	lua_pushcfunction(L,thread_recvmessage);
	lua_setglobal(L,"RecvMessage");

	//Load neccessary files
	loadLua(state,L,"metrostroi/sv_turbostroi.lua");
	loadLua(state,L,"metrostroi/sh_failsim.lua");
	
	//Load up all the systems
	char* systems = metrostroiSystemsList;
	while (systems[0]) {
		char filename[8192] = { 0 };
		char* system_name = systems;
		char* system_path = strchr(systems,'\n')+1;

		strncpy(filename,system_path,strchr(system_path,'\n')-system_path);
		loadLua(state,L,filename);		
		systems = system_path + strlen(filename) + 1;
	}

	//Initialize all the systems reported by the train
	systems = loadSystemsList;
	while (systems[0]) {
		char* system_name = systems;
		char* system_type = strchr(systems,'\n')+1;
		*(strchr(system_name,'\n')) = 0;
		*(strchr(system_type,'\n')) = 0;

		lua_getglobal(L,"LoadSystems");
		lua_pushstring(L,system_type);
		lua_setfield(L,-2,system_name);
		
		systems = system_type + strlen(system_type) + 1;
	}
	loadSystemsList[0] = 0;

	//Initialize systems
	lua_getglobal(L,"Initialize");
	if (lua_pcall(L,0,0,0)) {
		lua_pushcfunction(L,shared_print);
		lua_pushvalue(L,-2);
		lua_call(L,1,0);
		lua_pop(L,1);
	}

	//New userdata
	thread_userdata* userdata = (thread_userdata*)malloc(sizeof(thread_userdata));
	userdata->finished = 0;
	userdata->L = L;
	SIMC_Queue_Create(&userdata->thread_to_sim,QUEUE_SIZE,sizeof(thread_msg));
	SIMC_Queue_Create(&userdata->sim_to_thread,QUEUE_SIZE,sizeof(thread_msg));
	LUA->PushUserdata(userdata);
	LUA->SetField(1,"_sim_userdata");
	lua_pushlightuserdata(L,userdata);
	lua_setglobal(L,"_userdata");

	//Create thread for simulation
	SIMC_THREAD_ID thread = SIMC_Thread_Create(threadSimulation,userdata);
	return 0;
}

int API_DeinitializeTrain(lua_State* state) {
	LUA->GetField(1,"_sim_userdata");
	thread_userdata* userdata = (thread_userdata*)LUA->GetUserdata(-1);
	if (userdata) userdata->finished = 1;
	LUA->Pop();

	LUA->PushNil();
	LUA->SetField(1,"_sim_userdata");

	//Remove entry for train
	/*LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1,"Turbostroi");
	LUA->GetField(-1,"TrainData");
	LUA->Push(1);
	LUA->PushNil();
	LUA->SetTable(-3);
	LUA->Pop(); //TrainData
	LUA->Pop(); //Turbostroi
	LUA->Pop(); //GLOB*/
	return 0;
}

int API_LoadSystem(lua_State* state) {
	//char msg[8192] = { 0 };
	//LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	//LUA->GetField(-1,"Msg");
	//sprintf(msg,"Metrostroi: Loading system %s [%s]\n",LUA->GetString(1),LUA->GetString(2));
	//LUA->PushString(msg);
	//LUA->Call(1,0);
	//LUA->Pop();

	strncat(loadSystemsList,LUA->GetString(1),131071);
	strncat(loadSystemsList,"\n",131071);
	strncat(loadSystemsList,LUA->GetString(2),131071);
	strncat(loadSystemsList,"\n",131071);
	return 0;
}

int API_RegisterSystem(lua_State* state) {
	char msg[8192] = { 0 };
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1,"Msg");
	sprintf(msg,"Metrostroi: Registering system %s [%s]\n",LUA->GetString(1),LUA->GetString(2));
	LUA->PushString(msg);
	LUA->Call(1,0);
	LUA->Pop();

	strncat(metrostroiSystemsList,LUA->GetString(1),131071);
	strncat(metrostroiSystemsList,"\n",131071);
	strncat(metrostroiSystemsList,LUA->GetString(2),131071);
	strncat(metrostroiSystemsList,"\n",131071);
	return 0;
}

int API_Think(lua_State* state) {
	//Print pending message buffer
	if (printMessageBuf[0]) {
		SIMC_Lock_Enter(printMessageBufLock);
			LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
			LUA->GetField(-1,"MsgC");
			LUA->GetField(-2,"Color");
			LUA->PushNumber(255);
			LUA->PushNumber(0);
			LUA->PushNumber(255);
			LUA->Call(3,1);
			LUA->PushString(printMessageBuf);
			LUA->Call(2,0);
			LUA->Pop();
	
			printMessageBuf[0] = 0;
		SIMC_Lock_Leave(printMessageBufLock);
	}
	return 0;
}

int API_SendMessage(lua_State* state) {
	LUA->CheckType(2,Type::NUMBER);
	LUA->CheckType(3,Type::STRING);
	LUA->CheckType(4,Type::STRING);
	LUA->CheckType(5,Type::NUMBER);
	LUA->CheckType(6,Type::NUMBER);

	LUA->GetField(1,"_sim_userdata");
	thread_userdata* userdata = (thread_userdata*)LUA->GetUserdata(-1);
	LUA->Pop();

	if (userdata) {
		thread_msg* tmsg;
		SIMC_Queue_EnterWrite(userdata->sim_to_thread,(void**)&tmsg);
			tmsg->message = (int)LUA->GetNumber(2);
			strncpy(tmsg->system_name,LUA->GetString(3),63);
			tmsg->system_name[63] = 0;
			strncpy(tmsg->name,LUA->GetString(4),63);
			tmsg->name[63] = 0;
			tmsg->index = LUA->GetNumber(5);
			tmsg->value = LUA->GetNumber(6);
		SIMC_Queue_LeaveWrite(userdata->sim_to_thread);
	}
	return 0;
}

int API_RecvMessage(lua_State* state) {
	LUA->GetField(1,"_sim_userdata");
	thread_userdata* userdata = (thread_userdata*)LUA->GetUserdata(-1);
	LUA->Pop();

	if (userdata) {
		thread_msg* tmsg;
		if (SIMC_Queue_EnterRead(userdata->thread_to_sim,(void**)&tmsg)) {
			LUA->PushNumber(tmsg->message);
			LUA->PushString(tmsg->system_name);
			LUA->PushString(tmsg->name);
			LUA->PushNumber(tmsg->index);
			LUA->PushNumber(tmsg->value);
			SIMC_Queue_LeaveRead(userdata->thread_to_sim);
			return 5;
		}		
	}
	return 0;
}

int API_SetSimulationFPS(lua_State* state) {
	LUA->CheckType(1,Type::NUMBER);
	rate = LUA->GetNumber(1);
	return 0;
}

int API_SetTargetTime(lua_State* state) {
	LUA->CheckType(1,Type::NUMBER);
	target_time = LUA->GetNumber(1);
	return 0;
}


//------------------------------------------------------------------------------
// Initialization
//------------------------------------------------------------------------------
GMOD_MODULE_OPEN() {
	//Check whether being ran on server
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1,"SERVER");
	if (LUA->IsType(-1,Type::NIL)) {
		LUA->GetField(-2,"Msg");
		LUA->PushString("Metrostroi: DLL failed to initialize (gm_turbostroi.dll can only be used on server)\n");
		LUA->Call(1,0);
		return 0;
	}
	LUA->Pop(); //SERVER

	//Check for global table
	LUA->GetField(-1,"Metrostroi");
	if (LUA->IsType(-1,Type::NIL)) {
		LUA->GetField(-2,"Msg");
		LUA->PushString("Metrostroi: DLL failed to initialize (cannot be used standalone without metrostroi addon)\n");
		LUA->Call(1,0);
		return 0;
	}
	LUA->Pop(); //Metrostroi

	//Initialize threading
	SIMC_Thread_Initialize();
	printMessageBufLock = SIMC_Lock_Create();

	//Initialize API
	LUA->CreateTable();
	LUA->PushCFunction(API_InitializeTrain);
	LUA->SetField(-2,"InitializeTrain");
	LUA->PushCFunction(API_DeinitializeTrain);
	LUA->SetField(-2,"DeinitializeTrain");
	LUA->PushCFunction(API_Think);
	LUA->SetField(-2,"Think");
	LUA->PushCFunction(API_SendMessage);
	LUA->SetField(-2,"SendMessage");
	LUA->PushCFunction(API_RecvMessage);
	LUA->SetField(-2,"RecvMessage");
	LUA->PushCFunction(API_LoadSystem);
	LUA->SetField(-2,"LoadSystem");
	LUA->PushCFunction(API_RegisterSystem);
	LUA->SetField(-2,"RegisterSystem");
	LUA->PushCFunction(API_SetSimulationFPS);
	LUA->SetField(-2,"SetSimulationFPS");
	LUA->PushCFunction(API_SetTargetTime);
	LUA->SetField(-2,"SetTargetTime");

	LUA->SetField(-2,"Turbostroi");

	//Print some information
	LUA->GetField(-1,"Msg");
	LUA->Push(-1);

	LUA->PushString("Metrostroi: DLL initialized (built "__DATE__")\n");
	LUA->Call(1,0);

	char msg[8192] = { 0 };
	sprintf(msg,"Metrostroi: Running with %d processors/cores\n",SIMC_Thread_GetNumProcessors());
	LUA->PushString(msg);
	LUA->Call(1,0);
	LUA->Pop();
	return 0;
}


//------------------------------------------------------------------------------
// Deinitialization
//------------------------------------------------------------------------------
GMOD_MODULE_CLOSE() {
	SIMC_Lock_Destroy(printMessageBufLock);
	SIMC_Thread_Deinitialize();
	return 0;
}