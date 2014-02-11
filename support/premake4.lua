SIMC_STANDALONE = false
solution "gm_turbostroi"
   configurations {
     "Debug",
     "Release",
   }
   flags { "Symbols", "NoEditAndContinue", "NoPCH", "StaticRuntime", "EnableSSE" }
   --configuration "Debug*"
   --   defines { "DEBUG" }
   --configuration "Release*"
   
   location (_WORKING_DIR.."/"..(_ACTION or ""))
   targetdir ("E:\\Steam\\steamapps\\common\\GarrysMod\\garrysmod\\lua\\bin")
   targetsuffix "_win32"
   debugdir "../debug"
   debugargs {"-game garrysmod","-console","+map gm_metrostroi_b28" }
   
   -- Default configuration for libraries
   function library()
      kind "StaticLib"
      configuration {}
   end
   dofile("./../external/simc/support/premake4.lua")
   
-- Create working directory
if not os.isdir("../debug") then os.mkdir("../debug") end

-- Create file with path to Source 2013 SDK
--[[if not os.isfile("source_sdk_2013_path.txt") then
   local f = io.open("source_sdk_2013_path.txt","w+")
   f:write("E:\\Development\\HL2src2013")
   f:close()
end

-- Read Source 2013 SDK path
local f = io.open("source_sdk_2013_path.txt","r")
local SDK_PATH = f:read("*line")
f:close()

-- Don't do anything if source SDK is not setup
if not os.isdir(SDK_PATH) then
   print("Please define Source 2013 SDK path in source_sdk_2013_path.txt")
   return
end]]--

-- Create project
project "gmsv_turbostroi"
   uuid "C84AD4D2-2D63-1842-871E-41C8D71BEA58"
   kind "SharedLib"
   language "C++"

   defines { "GMMODULE" }
   includedirs {
     "../external/simc/include",
     "../external/garrysmod/include",
     "../external/luajit",
     "../source",
     --SDK_PATH.."/sp/src/public",
   }
   files {
     "../source/**.cpp",
     "../source/**.h",
   }
   libdirs {
     "../external/luajit",
     --SDK_PATH.."/sp/src/lib/public",
   }
   links { "simc", "lua51" } --"tier0",
