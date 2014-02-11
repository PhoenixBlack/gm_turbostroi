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
#define BUFFER_SIZE 1024*1024
double target_time = 0.0;
double rate = 100.0; //FPS
char metrostroiSystemsList[BUFFER_SIZE] = { 0 };
char loadSystemsList[BUFFER_SIZE] = { 0 };

typedef struct {
	double current_time;
	lua_State* L;
	int finished;

	SIMC_LOCK_ID readWriteLock;
	char readData[BUFFER_SIZE];
	char writeData[BUFFER_SIZE];
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

			//Perform data exchange
			SIMC_Lock_Enter(userdata->readWriteLock);
				lua_getglobal(L,"DataExchange");
				lua_pushstring(L,userdata->writeData);
				if (lua_pcall(L,1,1,0)) {
					lua_pushcfunction(L,shared_print);
					lua_pushvalue(L,-2);
					lua_call(L,1,0);
					lua_pop(L,1);
				}
				if (lua_tostring(L,-1)) {
					//userdata->readData[0] = 0; //This avoid overspam of messages
					if (((BUFFER_SIZE-1)-strlen(userdata->readData)) > strlen(lua_tostring(L,-1))) {
						strncat(userdata->readData,lua_tostring(L,-1),(BUFFER_SIZE-1) - strlen(lua_tostring(L,-1)));
					}
				} 
				lua_pop(L,1);
				userdata->writeData[0] = 0;
			SIMC_Lock_Leave(userdata->readWriteLock);

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
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1,"Turbostroi");
	LUA->GetField(-1,"TrainData");
	LUA->Push(1);
	LUA->PushString("");
	LUA->SetTable(-3);
	LUA->Pop(); //TrainData
	LUA->Pop(); //Turbostroi
	LUA->Pop(); //GLOB

	//Initialize LuaJIT for train
	lua_State* L  = luaL_newstate();
	luaL_openlibs(L);
	luaopen_bit(L);
	lua_pushboolean(L,1);
	lua_setglobal(L,"TURBOSTROI");
	lua_pushcfunction(L,shared_print);
	lua_setglobal(L,"print");

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
	userdata->readData[0] = 0;
	userdata->writeData[0] = 0;
	userdata->readWriteLock = SIMC_Lock_Create();
	LUA->PushUserdata(userdata);
	LUA->SetField(1,"_sim_userdata");

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
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1,"Turbostroi");
	LUA->GetField(-1,"TrainData");
	LUA->Push(1);
	LUA->PushNil();
	LUA->SetTable(-3);
	LUA->Pop(); //TrainData
	LUA->Pop(); //Turbostroi
	LUA->Pop(); //GLOB
	return 0;
}

int API_LoadSystem(lua_State* state) {
	char msg[8192] = { 0 };
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1,"Msg");
	sprintf(msg,"Metrostroi: Loading system %s [%s]\n",LUA->GetString(1),LUA->GetString(2));
	LUA->PushString(msg);
	LUA->Call(1,0);
	LUA->Pop();

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

int API_ExchangeData(lua_State* state) {
	LUA->CheckType(2,Type::STRING);

	LUA->GetField(1,"_sim_userdata");
	thread_userdata* userdata = (thread_userdata*)LUA->GetUserdata(-1);
	LUA->Pop();

	if (userdata) {
		SIMC_Lock_Enter(userdata->readWriteLock);
			//if ((131000-strlen(userdata->writeData)) > strlen(LUA->GetString(2))) {
				//strncat(userdata->writeData,LUA->GetString(2),131071);
			//}
			if (((BUFFER_SIZE-1)-strlen(userdata->writeData)) > strlen(LUA->GetString(2))) {
				strncat(userdata->writeData,LUA->GetString(2),(BUFFER_SIZE-1) - strlen(LUA->GetString(2)));
			}
			LUA->PushString(userdata->readData);
			userdata->readData[0] = 0;
		SIMC_Lock_Leave(userdata->readWriteLock);
	} else {
		LUA->PushString("");
	}
	return 1;
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
	LUA->Pop();

	//Check for global table
	LUA->GetField(-1,"Metrostroi");
	if (LUA->IsType(-1,Type::NIL)) {
		LUA->GetField(-2,"Msg");
		LUA->PushString("Metrostroi: DLL failed to initialize (cannot be used standalone without metrostroi addon)\n");
		LUA->Call(1,0);
		return 0;
	}
	LUA->Pop();

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
	LUA->PushCFunction(API_ExchangeData);
	LUA->SetField(-2,"ExchangeData");
	LUA->PushCFunction(API_LoadSystem);
	LUA->SetField(-2,"LoadSystem");
	LUA->PushCFunction(API_RegisterSystem);
	LUA->SetField(-2,"RegisterSystem");
	LUA->PushCFunction(API_SetSimulationFPS);
	LUA->SetField(-2,"SetSimulationFPS");
	LUA->PushCFunction(API_SetTargetTime);
	LUA->SetField(-2,"SetTargetTime");

	LUA->CreateTable();
	LUA->SetField(-2,"TrainData");

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