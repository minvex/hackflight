/*
   V-REP simulator plugin code 3D SLAM visualization

   Copyright (C) Simon D. Levy, Matt Lubas, and Alfredo Rwagaju 2016

   This file is part of 3DSLAM.

   3DSLAM is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   3DSLAM is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with 3DSLAM.  If not, see <http://www.gnu.org/licenses/>.
*/

static const char * PORTNAME = "/dev/ttyUSB0";
static const int   BAUDRATE = 57600;

#include "v_repExt.h"
#include "scriptFunctionData.h"
#include "v_repLib.h"

#include "msppg.h"
#include "serial.hpp"

#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>

#include <iostream>
using namespace std;

#define CONCAT(x,y,z) x y z
#define strConCat(x,y,z)	CONCAT(x,y,z)

#define PLUGIN_NAME  "3DSLAM"

LIBRARY vrepLib;

SerialConnection serialConnection(PORTNAME, BAUDRATE);

class My_ATTITUDE_Handler : public ATTITUDE_Handler {

    private:

        SerialConnection * serialConnection;

    public:

        My_ATTITUDE_Handler(SerialConnection * s) : ATTITUDE_Handler() {

            this->serialConnection = s;
        }

        void handle_ATTITUDE(short angx, short angy, short heading) {

            printf("%+3d %+3d %+3d\n", angx, angy, heading);

            this->sendAttitudeRequest();
        }

        void sendAttitudeRequest(void) {

            MSP_Message request = MSP_Parser::serialize_ATTITUDE_Request();

            for (byte b=request.start(); request.hasNext(); b=request.getNext())
                this->serialConnection->writeBytes((char *)&b, 1);
        }

};


// --------------------------------------------------------------------------------------
// simExt3DSLAM_start
// --------------------------------------------------------------------------------------
#define LUA_START_COMMAND  "simExt3DSLAM_start"

void LUA_START_CALLBACK(SScriptCallBack* cb)
{
    serialConnection.openConnection();

    MSP_Parser parser;

    MSP_Message request = MSP_Parser::serialize_ATTITUDE_Request();

    My_ATTITUDE_Handler handler(&serialConnection);

    parser.set_ATTITUDE_Handler(&handler);

    handler.sendAttitudeRequest();

    // Return success to V-REP
    CScriptFunctionData D;
    D.pushOutData(CScriptFunctionDataItem(true));
    D.writeDataToStack(cb->stackID);
}

// --------------------------------------------------------------------------------------
// simExt3DSLAM_update
// --------------------------------------------------------------------------------------

#define LUA_UPDATE_COMMAND "simExt3DSLAM_update"

void LUA_UPDATE_CALLBACK(SScriptCallBack* cb)
{
    /*

       cube = simCreatePureShape(0,          -- cube
       16,         -- static
       {CUBESIZE, CUBESIZE, CUBESIZE},
       0)          -- mass

   simSetObjectPosition(cube, -1, {x,y,z}) -- -1 = absolute position 

*/

    CScriptFunctionData D;

    // Return success to V-REP
    D.pushOutData(CScriptFunctionDataItem(true)); 
    D.writeDataToStack(cb->stackID);

} // LUA_UPDATE_COMMAND


// --------------------------------------------------------------------------------------
// simExt3DSLAM_stop
// --------------------------------------------------------------------------------------
#define LUA_STOP_COMMAND "simExt3DSLAM_stop"


void LUA_STOP_CALLBACK(SScriptCallBack* cb)
{
    serialConnection.closeConnection();

    CScriptFunctionData D;
    D.pushOutData(CScriptFunctionDataItem(true));
    D.writeDataToStack(cb->stackID);
}

// --------------------------------------------------------------------------------------



VREP_DLLEXPORT unsigned char v_repStart(void* reservedPointer,int reservedInt)
{ 
    char curDirAndFile[1024];

    getcwd(curDirAndFile, sizeof(curDirAndFile));

    std::string currentDirAndPath(curDirAndFile);
    std::string temp(currentDirAndPath);

    temp+="/libv_rep.so";

    vrepLib=loadVrepLibrary(temp.c_str());
    if (vrepLib==NULL)
    {
        std::cout << "Error, could not find or correctly load v_rep.dll. Cannot start '3DSLAM' plugin.\n";
        return(0); // Means error, V-REP will unload this plugin
    }
    if (getVrepProcAddresses(vrepLib)==0)
    {
        std::cout << "Error, could not find all required functions in v_rep plugin. Cannot start '3DSLAM' plugin.\n";
        unloadVrepLibrary(vrepLib);
        return(0); // Means error, V-REP will unload this plugin
    }

    // Check the V-REP version:
    int vrepVer;
    simGetIntegerParameter(sim_intparam_program_version,&vrepVer);
    if (vrepVer<30200) // if V-REP version is smaller than 3.02.00
    {
        std::cout << "Sorry, your V-REP copy is somewhat old, V-REP 3.2.0 or higher is required. Cannot start '3DSLAM' plugin.\n";
        unloadVrepLibrary(vrepLib);
        return(0); // Means error, V-REP will unload this plugin
    }

    // Register new Lua commands:
    simRegisterScriptCallbackFunction(strConCat(LUA_START_COMMAND,"@",PLUGIN_NAME),
            strConCat("boolean result=",LUA_START_COMMAND,
                "(number 3DSLAMHandle,number duration,boolean returnDirectly=false)"),LUA_START_CALLBACK);
    simRegisterScriptCallbackFunction(strConCat(LUA_UPDATE_COMMAND,"@",PLUGIN_NAME), NULL, LUA_UPDATE_CALLBACK);
    simRegisterScriptCallbackFunction(strConCat(LUA_STOP_COMMAND,"@",PLUGIN_NAME),
            strConCat("boolean result=",LUA_STOP_COMMAND,"(number 3DSLAMHandle)"),LUA_STOP_CALLBACK);

    return 8; // initialization went fine, we return the version number of this plugin (can be queried with simGetModuleName)
}

VREP_DLLEXPORT void v_repEnd()
{ // This is called just once, at the end of V-REP
    unloadVrepLibrary(vrepLib); // release the library
}

VREP_DLLEXPORT void* v_repMessage(int message, int * auxiliaryData, void * customData, int * replyData)
{
    while (serialConnection.bytesAvailable() > 0) {
        char c = 0;
        serialConnection.readBytes(&c, 1);
        printf("%02X\n", c);
    }

    int errorModeSaved;
    simGetIntegerParameter(sim_intparam_error_report_mode,&errorModeSaved);
    simSetIntegerParameter(sim_intparam_error_report_mode,sim_api_errormessage_ignore);
    simSetIntegerParameter(sim_intparam_error_report_mode,errorModeSaved); // restore previous settings

    return NULL;
}