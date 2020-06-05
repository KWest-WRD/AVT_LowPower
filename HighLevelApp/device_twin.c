#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <math.h>

// applibs_versions.h defines the API struct versions to use for applibs APIs.
#include "applibs_versions.h"
//#include "epoll_timerfd_utilities.h"

#include <applibs/log.h>
#include <applibs/i2c.h>
#include <applibs/gpio.h>

#include "hw/avnet_mt3620_sk.h"
#include "device_twin.h"
#include "azure_iot_utilities.h"
#include "parson.h"
#include "build_options.h"
#include "error_codes.h"

extern bool statusLedOn;
extern bool DoubleTapDetection;
extern int DoubleTapThreshold;
extern unsigned powerdownResidencyTime;
extern int TimeGapDoubleTapRecognition;
extern int QuietTimeAfterATapDetection;
extern int MaximumDurationOfOverthresholdEvent;

//extern volatile sig_atomic_t terminationRequired;
extern volatile sig_atomic_t exitCode;

static const char cstrDeviceTwinJsonInteger[] = "{\"%s\": %d}";
static const char cstrDeviceTwinJsonFloat[] = "{\"%s\": %.2f}";
static const char cstrDeviceTwinJsonBool[] = "{\"%s\": %s}";
static const char cstrDeviceTwinJsonString[] = "{\"%s\": \"%s\"}";

// Define the IoT Central device twin data objects
static const char cstrDeviceTwinJsonFloatIOTC[] = "{\"%s\": {\"value\": %.2f, \"status\" : \"completed\" , \"desiredVersion\" : %d }}";
static const char cstrDeviceTwinJsonBoolIOTC[] = "{\"%s\": {\"value\": %s, \"status\" : \"completed\" , \"desiredVersion\" : %d }}";
static const char cstrDeviceTwinJsonIntegerIOTC[] = "{\"%s\": {\"value\": %d, \"status\" : \"completed\" , \"desiredVersion\" : %d }}";
static const char cstrDeviceTwinJsonStringIOTC[]  = "{\"%s\": {\"value\": \"%s\", \"status\" : \"completed\" , \"desiredVersion\" : %d }}";

static int desiredVersion = 0;

// Define each device twin key that we plan to catch, process, and send reported property for.
// .twinKey - The JSON Key piece of the key: value pair
// .twinVar - The address of the application variable keep this key: value pair data
// .twinFD - The associated File Descriptor for this item.  This is usually a GPIO FD.  NULL if NA.
// .twinGPIO - The associted GPIO number for this item.  NO_GPIO_ASSOCIATED_WITH_TWIN if NA
// .twinType - The data type for this item, TYPE_BOOL, TYPE_STRING, TYPE_INT, or TYPE_FLOAT
// .active_high - true if GPIO item is active high, false if active low.  This is used to init the GPIO 
twin_t twinArray[7] = {
	{.twinKey = "SleepTime",.twinVar = &powerdownResidencyTime,.twinFd = NULL,.twinGPIO = NO_GPIO_ASSOCIATED_WITH_TWIN,.twinType = TYPE_INT,.active_high = false},
	{.twinKey = "DoubleTapDetection",.twinVar = &DoubleTapDetection,.twinFd = NULL,.twinGPIO = NO_GPIO_ASSOCIATED_WITH_TWIN,.twinType = TYPE_BOOL,.active_high = false},
	{.twinKey = "DoubleTapThreshold",.twinVar = &DoubleTapThreshold,.twinFd = NULL,.twinGPIO = NO_GPIO_ASSOCIATED_WITH_TWIN,.twinType = TYPE_INT,.active_high = false},
	{.twinKey = "TimeGapForDoubleTapRecognition",.twinVar = &TimeGapDoubleTapRecognition,.twinFd = NULL,.twinGPIO = NO_GPIO_ASSOCIATED_WITH_TWIN,.twinType = TYPE_INT,.active_high = false},
	{.twinKey = "QuietTimeAfterATapDetection",.twinVar = &QuietTimeAfterATapDetection,.twinFd = NULL,.twinGPIO = NO_GPIO_ASSOCIATED_WITH_TWIN,.twinType = TYPE_INT,.active_high = false},
	{.twinKey = "MaximumDurationOfOverThresholdEvent",.twinVar = &MaximumDurationOfOverthresholdEvent,.twinFd = NULL,.twinGPIO = NO_GPIO_ASSOCIATED_WITH_TWIN,.twinType = TYPE_INT,.active_high = false},
};

// Calculate how many twin_t items are in the array.  We use this to iterate through the structure.
int twinArraySize = sizeof(twinArray) / sizeof(twin_t);

///<summary>
///		check to see if any of the device twin properties have been updated.  If so, send up the current data.
///</summary>
void checkAndUpdateDeviceTwin(char* property, void* value, data_type_t type, bool ioTCentralFormat)
{
	int nJsonLength = -1;

	char *pjsonBuffer = (char *)malloc(JSON_BUFFER_SIZE);
	if (pjsonBuffer == NULL) {
		Log_Debug("ERROR: not enough memory to report device twin changes.");
	}

	if (property != NULL) {

		// report current device twin data as reported properties to IoTHub

		switch (type) {
			case TYPE_BOOL:
				nJsonLength = snprintf(pjsonBuffer, JSON_BUFFER_SIZE, ioTCentralFormat ? cstrDeviceTwinJsonBoolIOTC : cstrDeviceTwinJsonBool, property, *(bool*)value ? "true" : "false", desiredVersion);
				break;
			case TYPE_FLOAT:
					nJsonLength = snprintf(pjsonBuffer, JSON_BUFFER_SIZE, ioTCentralFormat ? cstrDeviceTwinJsonFloatIOTC : cstrDeviceTwinJsonFloat, property, *(float*)value, desiredVersion);
				break;
			case TYPE_INT:
				nJsonLength = snprintf(pjsonBuffer, JSON_BUFFER_SIZE, ioTCentralFormat ? cstrDeviceTwinJsonIntegerIOTC : cstrDeviceTwinJsonInteger, property, *(int*)value, desiredVersion);
				break;
			case TYPE_STRING:
				nJsonLength = snprintf(pjsonBuffer, JSON_BUFFER_SIZE, ioTCentralFormat ? cstrDeviceTwinJsonStringIOTC : cstrDeviceTwinJsonString, property, (char*)value, desiredVersion);
				break;
		}

		if (nJsonLength > 0) {
			Log_Debug("[MCU] Updating device twin: %s\n", pjsonBuffer);
			AzureIoT_TwinReportStateJson(pjsonBuffer, (size_t)nJsonLength);
		}
		free(pjsonBuffer);
	}
}

///<summary>
///		Parses received desired property changes.
///</summary>
///<param name="desiredProperties">Address of desired properties JSON_Object</param>
void deviceTwinChangedHandler(JSON_Object * desiredProperties)
{
	int result = 0;
#ifdef IOT_CENTRAL_APPLICATION	
	const bool sendIoTCentralFormat = true;
#else
	const bool sendIoTCentralFormat = false;
#endif 

	// Pull the twin version out of the message.  We use this value when we echo the new setting back to IoT Connect.
	if (json_object_has_value(desiredProperties, "$version") != 0)
	{
		desiredVersion = (int)json_object_get_number(desiredProperties, "$version");
	}

	for (int i = 0; i < (sizeof(twinArray) / sizeof(twin_t)); i++) {

		if (json_object_has_value(desiredProperties, twinArray[i].twinKey) != 0)
		{

			switch (twinArray[i].twinType) {
			case TYPE_BOOL:
				*(bool*)twinArray[i].twinVar = (bool)json_object_get_boolean(desiredProperties, twinArray[i].twinKey);
				
				// Check to see if we're operating on a GPIO.  If not, then skip over the GPIO specific code
				if (twinArray[i].twinGPIO != NO_GPIO_ASSOCIATED_WITH_TWIN) {
					result = GPIO_SetValue(*twinArray[i].twinFd, twinArray[i].active_high ? (GPIO_Value) * (bool*)twinArray[i].twinVar : !(GPIO_Value) * (bool*)twinArray[i].twinVar);

					if (result != 0) {
						Log_Debug("Fd: %d\n", twinArray[i].twinFd);
						Log_Debug("FAILURE: Could not set GPIO_%d, %d output value %d: %s (%d).\n", twinArray[i].twinGPIO, twinArray[i].twinFd, (GPIO_Value) * (bool*)twinArray[i].twinVar, strerror(errno), errno);
						exitCode = ExitCode_TermHandler_SigTerm;
					}
				}
				Log_Debug("Received device update. New %s is %s\n", twinArray[i].twinKey, *(bool*)twinArray[i].twinVar ? "true" : "false");
				checkAndUpdateDeviceTwin(twinArray[i].twinKey, twinArray[i].twinVar, TYPE_BOOL, sendIoTCentralFormat);
				break;
			case TYPE_FLOAT:
				*(float*)twinArray[i].twinVar = (float)json_object_get_number(desiredProperties, twinArray[i].twinKey);
				Log_Debug("Received device update. New %s is %0.2f\n", twinArray[i].twinKey, *(float*)twinArray[i].twinVar);
				checkAndUpdateDeviceTwin(twinArray[i].twinKey, twinArray[i].twinVar, TYPE_FLOAT, sendIoTCentralFormat);
				break;
			case TYPE_INT:
				*(int*)twinArray[i].twinVar = (int)json_object_get_number(desiredProperties, twinArray[i].twinKey);
				Log_Debug("Received device update. New %s is %d\n", twinArray[i].twinKey, *(int*)twinArray[i].twinVar);
				checkAndUpdateDeviceTwin(twinArray[i].twinKey, twinArray[i].twinVar, TYPE_INT, sendIoTCentralFormat);
				break;

			case TYPE_STRING:
				strcpy((char*)twinArray[i].twinVar, (char*)json_object_get_string(desiredProperties, twinArray[i].twinKey));
				Log_Debug("Received device update. New %s is %s\n", twinArray[i].twinKey, (char*)twinArray[i].twinVar);
				checkAndUpdateDeviceTwin(twinArray[i].twinKey, twinArray[i].twinVar, TYPE_STRING, sendIoTCentralFormat);
				break;
			}
		}
	}
}
