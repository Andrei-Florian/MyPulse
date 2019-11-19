/*
	MyPulse
	17/NOV/2019 - 19/NOV/2019 | Andrei Florian
*/

#include <applibs/log.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h> 
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <math.h>

#include "applibs_versions.h"
#include "epoll_timerfd_utilities.h"
#include "mt3620_avnet_dev.h"
#include "deviceTwin.h"
#include "azure_iot_utilities.h"
#include "connection_strings.h"
#include "build_options.h"

#include <applibs/i2c.h>
#include <applibs/gpio.h>
#include <applibs/wificonfig.h>
#include <azureiot/iothub_device_client_ll.h>

#include <sys/time.h>
#include "applibs_versions.h"
#include "max30102.h"
#include "algorithm_by_RF.h"
#include "mt3620_rdb.h"

#include "i2c.h"
#include "lsm6dso_reg.h"
#include "lps22hh_reg.h"

#include <applibs/rtc.h>
#include <applibs/networking.h>

#include "universum_utilities.h";

#define PROXIMITY_THRESHOLD  32000
#define MIKROE_INT MT3620_GPIO2

// User Changable
int run_time = 30;  //default to 30 second run time

// WHO Health Values
minRate = 60;
maxRate = 100;
minOxygen = 95;

// button
bool buttonState = false;
int buttonAOpen; // button pin

static int i2cFd = -1;
static int intPinFd = -1;

// IoT Hub credentials
bool networkConfigSent = false;
char ssid[128];
uint32_t frequency;
char bssid[20];

// Provide local access to variables in other files
extern twin_t twinArray[];
extern int twinArraySize;
extern IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle;
extern int accelTimerFd;

// Support functions.
static void TerminationHandler(int signalNumber);
static int InitPeripheralsAndHandlers(void);
static void ClosePeripheralsAndHandlers(void);

// File descriptors - initialized to invalid value
int epollFd = -1;
bool versionStringSent = false;

// Define the Json string format
static const char jsonFormat[45] = "{\"%s\":\"%f\", \"%s\":\"%i\", \"%s\":\"%f\", \"%s\":\"%f\"}";

// Termination state
volatile sig_atomic_t terminationRequired = false;

// Hr4 Variables
float avgOxygen = 0;
int avgRate = 0;

// temperature variables
lsm6dso_ctx_t dev_ctx;
float temp = 0;

// pressure variables
lps22hh_ctx_t pressure_ctx;
float atmoPressure = 0;

// time variables
struct timeval time_start, time_now;
int runtime = 60; // runtime in seconds

// pins
int r;
int g;
int b;
int buzzer;

// Signal handler for termination requests. This handler must be async-signal-safe
static void TerminationHandler(int signalNumber)
{
	terminationRequired = true;
}

// Allocates and formats a string message on the heap
static void *SetupHeapMessage(const char *messageFormat, size_t maxLength, ...)
{
	va_list args;
	va_start(args, maxLength);
	char *message =
		malloc(maxLength + 1); // Ensure there is space for the null terminator put by vsnprintf.
	if (message != NULL) {
		vsnprintf(message, maxLength, messageFormat, args);
	}
	va_end(args);
	return message;
}

// Direct Method callback function, called when a Direct Method call is received from the Azure IoT Hub
static int DirectMethodCall(const char *methodName, const char *payload, size_t payloadSize, char **responsePayload, size_t *responsePayloadSize)
{
	Log_Debug("\nDirect Method called %s\n", methodName);

	int result = 404; // HTTP status code.

	if (payloadSize < 32) 
	{
		// Declare a char buffer on the stack where we'll operate on a copy of the payload.  
		char directMethodCallContent[payloadSize + 1];

		// Prepare the payload for the response. This is a heap allocated null terminated string.
		// The Azure IoT Hub SDK is responsible of freeing it.
		*responsePayload = NULL;  // Reponse payload content.
		*responsePayloadSize = 0; // Response payload content size.


		// Look for the haltApplication method name.  This direct method does not require any payload, other than
		// a valid Json argument such as {}.

		if (strcmp(methodName, "haltApplication") == 0) 
		{
			// Log that the direct method was called and set the result to reflect success!
			Log_Debug("haltApplication() Direct Method called\n");
			result = 200;

			// Construct the response message.  This response will be displayed in the cloud when calling the direct method
			static const char resetOkResponse[] =
				"{ \"success\" : true, \"message\" : \"Halting Application\" }";
			size_t responseMaxLength = sizeof(resetOkResponse);
			*responsePayload = SetupHeapMessage(resetOkResponse, responseMaxLength);
			if (*responsePayload == NULL) {
				Log_Debug("ERROR: Could not allocate buffer for direct method response payload.\n");
				abort();
			}
			*responsePayloadSize = strlen(*responsePayload);

			// Set the terminitation flag to true.  When in Visual Studio this will simply halt the application.
			// If this application was running with the device in field-prep mode, the application would halt
			// and the OS services would resetart the application.
			terminationRequired = true;
			return result;
		}

		// Check to see if the setSensorPollTime direct method was called
		else if (strcmp(methodName, "setSensorPollTime") == 0) 
		{

			// Log that the direct method was called and set the result to reflect success!
			Log_Debug("setSensorPollTime() Direct Method called\n");
			result = 200;

			// The payload should contain a JSON object such as: {"pollTime": 20}
			if (directMethodCallContent == NULL) 
			{
				Log_Debug("ERROR: Could not allocate buffer for direct method request payload.\n");
				abort();
			}

			// Copy the payload into our local buffer then null terminate it.
			memcpy(directMethodCallContent, payload, payloadSize);
			directMethodCallContent[payloadSize] = 0; // Null terminated string.

			JSON_Value *payloadJson = json_parse_string(directMethodCallContent);

			// Verify we have a valid JSON string from the payload
			if (payloadJson == NULL) 
			{
				goto payloadError;
			}

			// Verify that the payloadJson contains a valid JSON object
			JSON_Object *pollTimeJson = json_value_get_object(payloadJson);
			if (pollTimeJson == NULL) 
			{
				goto payloadError;
			}

			// Pull the Key: value pair from the JSON object, we're looking for {"pollTime": <integer>}
			// Verify that the new timer is < 0
			int newPollTime = (int)json_object_get_number(pollTimeJson, "pollTime");
			if (newPollTime < 1) 
			{
				goto payloadError;
			}
			else 
			{

				Log_Debug("New PollTime %d\n", newPollTime);

				// Construct the response message.  This will be displayed in the cloud when calling the direct method
				static const char newPollTimeResponse[] =
					"{ \"success\" : true, \"message\" : \"New Sensor Poll Time %d seconds\" }";
				size_t responseMaxLength = sizeof(newPollTimeResponse) + strlen(payload);
				*responsePayload = SetupHeapMessage(newPollTimeResponse, responseMaxLength, newPollTime);
				if (*responsePayload == NULL) 
				{
					Log_Debug("ERROR: Could not allocate buffer for direct method response payload.\n");
					abort();
				}
				*responsePayloadSize = strlen(*responsePayload);

				// Define a new timespec variable for the timer and change the timer period
				struct timespec newAccelReadPeriod = { .tv_sec = newPollTime,.tv_nsec = 0 };
				SetTimerFdToPeriod(accelTimerFd, &newAccelReadPeriod);
				return result;
			}
		}
		else 
		{
			result = 404;
			Log_Debug("INFO: Direct Method called \"%s\" not found.\n", methodName);

			static const char noMethodFound[] = "\"method not found '%s'\"";
			size_t responseMaxLength = sizeof(noMethodFound) + strlen(methodName);
			*responsePayload = SetupHeapMessage(noMethodFound, responseMaxLength, methodName);
			if (*responsePayload == NULL) 
			{
				Log_Debug("ERROR: Could not allocate buffer for direct method response payload.\n");
				abort();
			}
			*responsePayloadSize = strlen(*responsePayload);
			return result;
		}

	}
	else 
	{
		Log_Debug("Payload size > 32 bytes, aborting Direct Method execution\n");
		goto payloadError;
	}

	// If there was a payload error, construct the 
	// response message and send it back to the IoT Hub for the user to see
	payloadError:


	result = 400; // Bad request.
	Log_Debug("INFO: Unrecognised direct method payload format.\n");

	static const char noPayloadResponse[] =
		"{ \"success\" : false, \"message\" : \"request does not contain an identifiable "
		"payload\" }";

	size_t responseMaxLength = sizeof(noPayloadResponse) + strlen(payload);
	responseMaxLength = sizeof(noPayloadResponse);
	*responsePayload = SetupHeapMessage(noPayloadResponse, responseMaxLength);
	if (*responsePayload == NULL) 
	{
		Log_Debug("ERROR: Could not allocate buffer for direct method response payload.\n");
		abort();
	}

	*responsePayloadSize = strlen(*responsePayload);

	return result;
}

int read_i2c(uint8_t addr, uint16_t count, uint8_t* ptr)
{
	int r = I2CMaster_WriteThenRead(i2cFd, MAX30101_SAD, &addr, sizeof(addr), ptr, count);
	if (r == -1)
		Log_Debug("ERROR: I2CMaster_Writer: errno=%d (%s)\n", errno, strerror(errno));
	return r;
}

void write_i2c(uint8_t addr, uint16_t count, uint8_t* ptr)
{
	uint8_t buff[2];
	buff[0] = addr;
	buff[1] = *ptr;

	int r = I2CMaster_Write(i2cFd, MAX30101_SAD, buff, 2);
	if (r == -1)
		Log_Debug("ERROR: I2CMaster_Writer: errno=%d (%s)\n", errno, strerror(errno));
}

int getButton() // this loop checks if the button is pressed
{
	GPIO_Value_Type newButtonAState;
	int result = GPIO_GetValue(buttonAOpen, &newButtonAState); // read the GPIO value

	if (result < 0) // if read was not allowed
	{
		Log_Debug("  [ERROR]  Access to Pin Denied \n");
		return -1;
	}

	return newButtonAState;
}

// Initialises the GPIO connections
void setConnections()
{
	Log_Debug("[Setup] Opening GPIO R \n");
	r = GPIO_OpenAsOutput(1, GPIO_OutputMode_PushPull, GPIO_Value_Low); // open the GPIO pin as an output

	// check if the pin was opened successfully
	if (r < 0)
	{
		Log_Debug("[Setup] GPIO Failed to Open \n");
	}

	Log_Debug("[Setup] GPIO is Open \n");

	Log_Debug("[Setup] Opening GPIO G \n");
	g = GPIO_OpenAsOutput(31, GPIO_OutputMode_PushPull, GPIO_Value_Low); // open the GPIO pin as an output

	// check if the pin was opened successfully
	if (g < 0)
	{
		Log_Debug("[Setup] GPIO Failed to Open \n");
	}

	Log_Debug("[Setup] GPIO is Open \n");

	Log_Debug("[Setup] Opening GPIO B \n");
	b = GPIO_OpenAsOutput(35, GPIO_OutputMode_PushPull, GPIO_Value_Low); // open the GPIO pin as an output

	// check if the pin was opened successfully
	if (b < 0)
	{
		Log_Debug("[Setup] GPIO Failed to Open \n");
	}

	Log_Debug("[Setup] GPIO is Open \n");

	Log_Debug("[Setup] Opening GPIO Buzz\n");
	buzzer = GPIO_OpenAsOutput(43, GPIO_OutputMode_PushPull, GPIO_Value_Low); // open the GPIO pin as an output

	// check if the pin was opened successfully
	if (buzzer < 0)
	{
		Log_Debug("[Setup] GPIO Failed to Open \n");
	}

	Log_Debug("[Setup] GPIO is Open \n");
}

// Set up SIGTERM termination handler, initialize peripherals, and set up event handlers
static int InitPeripheralsAndHandlers(void)
{
	Log_Debug("[Setup] Opening Button \n");
	buttonAOpen = GPIO_OpenAsInput(17); // open the GPIO pin as an output

	// check if the pin was opened successfully
	if (buttonAOpen < 0)
	{
		Log_Debug("[Setup] Error - GPIO Failed to Open \n");
		return -1;
	}
	Log_Debug("[Setup] Success - Button is Open \n");

	println("[Setup] Setting up handlers");
	struct sigaction action;
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = TerminationHandler;
	sigaction(SIGTERM, &action, NULL);

	epollFd = CreateEpollFd();
	if (epollFd < 0) 
	{
		return -1;
	}

	println("[Setup] Initialising I2C");

	Log_Debug("[Setup] Setting Up Temp Sensor Side \n");
	if (initI2c() == -1) // initialise the I2C interface for onboard sensors and MikroE
	{
		return -1;
	}

	intPinFd = GPIO_OpenAsInput(MIKROE_INT);

	i2cFd = I2CMaster_Open(MT3620_RDB_HEADER4_ISU2_I2C);
	if (i2cFd < 0)
	{
		Log_Debug("[Setup] ERROR: I2CMaster_Open: errno=%d (%s)\n", errno, strerror(errno));
		return;
	}

	int result = I2CMaster_SetBusSpeed(i2cFd, I2C_BUS_SPEED_STANDARD);
	if (result != 0)
	{
		Log_Debug("[Setup] ERROR: I2CMaster_SetBusSpeed: errno=%d (%s)\n", errno, strerror(errno));
		return;
	}

	result = I2CMaster_SetTimeout(i2cFd, 100);
	if (result != 0)
	{
		Log_Debug("[Setup] ERROR: I2CMaster_SetTimeout: errno=%d (%s)\n", errno, strerror(errno));
		return;
	}

	setConnections(); // initialise GPIO connections
	println("[Setup] Setting up Device Twin");
	// Tell the system about the callback function that gets called when we receive a device twin update message from Azure
	AzureIoT_SetDeviceTwinUpdateCallback(&deviceTwinChangedHandler);

	return 0;
}

// Close peripherals and handlers
static void ClosePeripheralsAndHandlers(void)
{
	Log_Debug("Closing file descriptors.\n");
	CloseFdAndPrintError(epollFd, "Epoll");
}

// keep the connection with the cloud online and send telementry data
void dowork()
{
	if (!AzureIoT_SetupClient())
	{
		Log_Debug("[Setup] ERROR: Failed to set up IoT Hub client\n");
		while (1);
	}

	WifiConfig_ConnectedNetwork network;
	int result = WifiConfig_GetCurrentNetwork(&network);

	if (result < 0)
	{
		Log_Debug("INFO: Not currently connected to a WiFi network.\n");
	}
	else
	{
		frequency = network.frequencyMHz;
		snprintf(bssid, JSON_BUFFER_SIZE, "%02x:%02x:%02x:%02x:%02x:%02x",
			network.bssid[0], network.bssid[1], network.bssid[2],
			network.bssid[3], network.bssid[4], network.bssid[5]);

		if ((strncmp(ssid, (char*)&network.ssid, network.ssidLength) != 0) || !networkConfigSent)
		{
			memset(ssid, 0, 128);
			strncpy(ssid, network.ssid, network.ssidLength);
			Log_Debug("SSID: %s\n", ssid);
			Log_Debug("Frequency: %dMHz\n", frequency);
			Log_Debug("bssid: %s\n", bssid);
			networkConfigSent = true;

			// Note that we send up this data to Azure if it changes, but the IoT Central Properties elements only 
			// show the data that was currenet when the device first connected to Azure.
			checkAndUpdateDeviceTwin("ssid", &ssid, TYPE_STRING, false);
			checkAndUpdateDeviceTwin("freq", &frequency, TYPE_INT, false);
			checkAndUpdateDeviceTwin("bssid", &bssid, TYPE_STRING, false);
		}
	}

	// AzureIoT_DoPeriodicTasks() needs to be called frequently in order to keep active
	// the flow of data with the Azure IoT Hub
	AzureIoT_DoPeriodicTasks();
	delay(100);
}

// turn LED red
void ledRed()
{
	GPIO_SetValue(r, GPIO_Value_High);
	GPIO_SetValue(g, GPIO_Value_Low);
	GPIO_SetValue(b, GPIO_Value_Low);
}

// turn LED blue
void ledBlue()
{
	GPIO_SetValue(b, GPIO_Value_High);
	GPIO_SetValue(g, GPIO_Value_Low);
	GPIO_SetValue(r, GPIO_Value_Low);
}

// turn LED purple
void ledPurple()
{
	GPIO_SetValue(b, GPIO_Value_High);
	GPIO_SetValue(g, GPIO_Value_Low);
	GPIO_SetValue(r, GPIO_Value_High);
}

// turn LED green
void ledGreen()
{
	GPIO_SetValue(b, GPIO_Value_Low);
	GPIO_SetValue(g, GPIO_Value_High);
	GPIO_SetValue(r, GPIO_Value_Low);
}

// turn LED orange
void ledOrange()
{
	GPIO_SetValue(b, GPIO_Value_Low);
	GPIO_SetValue(g, GPIO_Value_High);
	GPIO_SetValue(r, GPIO_Value_High);
}

// turn LED white
void ledWhite()
{
	GPIO_SetValue(b, GPIO_Value_High);
	GPIO_SetValue(g, GPIO_Value_High);
	GPIO_SetValue(r, GPIO_Value_Low);
}

// turn LED off
void ledOff() 
{
	GPIO_SetValue(b, GPIO_Value_Low);
	GPIO_SetValue(r, GPIO_Value_Low);
}

// buzz the buzzer for a defined amount of time
void buzz(float time)
{
	GPIO_SetValue(buzzer, GPIO_Value_High);
	delay(time);
	GPIO_SetValue(buzzer, GPIO_Value_Low);
}

// wait until button is pressed and then sample Hr
bool getHeartRate()
{
	println("[Program] Preapring Sample");
	float n_spo2 = 0;
	float ratio = 0;
	float correl = 0;
	int8_t   ch_spo2_valid = 0; //indicator to show if the SPO2 calculation is valid
	int32_t  n_heart_rate = 0; //heart rate value
	int8_t   ch_hr_valid = 0; //indicator to show if the heart rate calculation is valid
	uint32_t aun_ir_buffer[BUFFER_SIZE]; //infrared LED sensor data
	uint32_t aun_red_buffer[BUFFER_SIZE]; //red LED sensor data
	int32_t  i = 0;
	int32_t  average_hr = 0;
	float    average_spo2 = 0;
	int32_t  nbr_readings = 0;

	struct timeval time_start;
	struct timeval time_now;

	bool turn = false;

	memset(aun_ir_buffer, 0, 128);
	memset(aun_red_buffer, 0, 128);

	println("[Program] Press Button to get heart rate");
	while (!getButton()) // wait until the button is pressed.
	{
		dowork(); // buffer events in the meanwhile
		delay(200);
	}

	buzz(1000); // buzzer
	ledBlue();
	println("[Program] Initialising MikroE Device");
	maxim_max30102_i2c_setup(read_i2c, write_i2c);
	maxim_max30102_init(); // initialise the sensor and send the on command

	Log_Debug("[Program] Running test for %d seconds\n", run_time);
	println("[Program] Place Finger on Sensor - Starting in 5 seconds");
	delay(5000);

	println("");
	println("[Program] Reading Heart Rate");

	// get the start time of the sampling
	gettimeofday(&time_start, NULL);
	time_now = time_start;
	average_hr = nbr_readings = 0;
	average_spo2 = 0.0;

	while (difftime(time_now.tv_sec, time_start.tv_sec) < run_time) // run for defined time
	{
		// blink the blue LED
		if (turn)
		{
			ledBlue();
			turn = false;
		}
		else
		{
			ledOff();
			turn = true;
		}

		GPIO_Value_Type intVal;
		for (i = 0; i < BUFFER_SIZE; i++)
		{
			do
			{
				GPIO_GetValue(intPinFd, &intVal);
			} while (intVal == 1);                               //wait until the interrupt pin asserts

			maxim_max30102_read_fifo((aun_red_buffer + i), (aun_ir_buffer + i));   //read from MAX30102 FIFO
			Log_Debug(".");
		}
		Log_Debug("\n");
		//calculate heart rate and SpO2 after BUFFER_SIZE samples (ST seconds of samples) using Robert's method
		rf_heart_rate_and_oxygen_saturation(aun_ir_buffer, BUFFER_SIZE, aun_red_buffer, &n_spo2, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid, &ratio, &correl);

		if (ch_hr_valid && ch_spo2_valid) // if sample is valid
		{
			println("[Program] Success - Sample is Good");
			Log_Debug("Blood Oxygen Level %.2f%% \n", n_spo2);
			Log_Debug("Heart Rate %d BPM \n", n_heart_rate);
			println("");
			average_hr += n_heart_rate;
			average_spo2 += n_spo2;
			nbr_readings++;
		}
		else
		{
			println("[Program] Sample is Bad");
		}
		gettimeofday(&time_now, NULL); // refresh the time
	}

	println("");
	println("");
	println("[Program] Samples Collated");

	// process the sample
	avgOxygen = average_spo2 / (float)nbr_readings;
	avgRate = average_hr / nbr_readings;

	// playing around with the values for testing
	//avgOxygen = 66;
	//avgRate = 40;

	buzz(1000);
	Log_Debug("Blood Oxygen Level %.2f%% \n", avgOxygen);
	Log_Debug("Heart Rate %d BPM \n", avgRate);
	max301024_shut_down(1);
	
	// ensure that the data is not corrupted again
	println("[Program] Veryfying Data");
	if (avgOxygen != 0 && avgRate != 0)
	{
		delay(1000);
		println("");
		println("[Program] Sample is Good");
		println("[Program] Moving On");
		println("");

		max301024_shut_down(1);
		return true;
	}
	else
	{
		delay(1000);
		println("[Program] Error - All Samples are Corrupted");
		println("[Program] Not sending data to Azure");

		for (int i = 0; i < 2; i++)
		{
			ledOff();
			buzz(1000);
			ledPurple();
			delay(1000);
		}
		max301024_shut_down(1);
		return false;
	}
}

// analyse the data to check if vitals are OK
void analyseData()
{
	println("[Program] Checking Vitals against WHO healthy standards");
	println("[Program] Checking Heart Rate");
	int problem = 0;

	if (avgRate < minRate || avgRate > maxRate) // compare heart rate with WHO standards
	{
		println("[Warning] Heart Rate is not normal [60bpm - 100bmp]");
		problem = 1;
	}
	else
	{
		println("[Info] Heart Rate is normal");
	}

	delay(1000);
	println("[Program] Checking Oxygen");
	if (avgOxygen < minOxygen) // compare SpO2 with WHO standards
	{
		println("[Warning] SpO2 is not normal [95 - 100]");
		if (problem == 1)
		{
			problem = 2;
		}
		else
		{
			problem = 3;
		}
	}
	else
	{
		println("[Info] SpO2 is normal");
	}

	// check if there was a problem
	if (problem == 0)
	{
		ledGreen();
	}
	else
	{
		for (int i = 0; i < 5; i++)
		{
			ledOff();
			buzz(1000);
			ledRed();
			delay(1000);
		}
	}

	delay(2000);
}

// get the temperature from the onboard sensor
void getTemp()
{
	uint8_t reg;

	static axis1bit16_t data_raw_temperature;
	lsm6dso_temp_flag_data_ready_get(&dev_ctx, &reg);

	if (reg)
	{
		memset(data_raw_temperature.u8bit, 0x00, sizeof(int16_t));
		lsm6dso_temperature_raw_get(&dev_ctx, data_raw_temperature.u8bit);
		temp = lsm6dso_from_lsb_to_celsius(data_raw_temperature.i16bit);

		Log_Debug("[INFO] Temperature %.2fC \n", temp);
	}
	else
	{
		for (int i = 0; i < 2; i++)
		{
			ledOff();
			buzz(1000);
			ledPurple();
			delay(1000);
		}
	}
}

// get the atmospheric pressure from the sensor
void getAtmoPressure()
{
	uint8_t reg;

	lps22hh_reg_t lps22hhReg;
	static axis1bit32_t data_raw_pressure;

	lps22hh_read_reg(&pressure_ctx, LPS22HH_STATUS, (uint8_t*)& lps22hhReg, 1);

	if ((lps22hhReg.status.p_da == 1) && (lps22hhReg.status.t_da == 1))
	{
		memset(data_raw_pressure.u8bit, 0x00, sizeof(int32_t));
		lps22hh_pressure_raw_get(&pressure_ctx, data_raw_pressure.u8bit);
		atmoPressure = lps22hh_from_lsb_to_hpa(data_raw_pressure.i32bit);

		Log_Debug("[INFO] Atmospheric Pressure %.2f hPa \n", atmoPressure);
	}
	else
	{
		for (int i = 0; i < 2; i++)
		{
			ledOff();
			buzz(1000);
			ledPurple();
			delay(1000);
		}
	}
}

// parse the data to Azure
void sendDataToAzure()
{
	delay(1000);
	println("[Program] Formatting Data to Send to Azure");

	char *pjsonBuffer = (char *)malloc(JSON_BUFFER_SIZE); // format a buffer
	if (pjsonBuffer == NULL)
	{
		Log_Debug("ERROR: not enough memory to send telemetry");
		for (int i = 0; i < 2; i++)
		{
			ledOff();
			buzz(1000);
			ledPurple();
			delay(1000);
		}
	}

	// append data to the buffer
	// snprintf(bufferToSend, 128, JSON Payload Config (defined above), dataLable, dataValue);
	snprintf(pjsonBuffer, JSON_BUFFER_SIZE, jsonFormat, "Oxygen", avgOxygen, "HeartRate", avgRate, "Temperature", temp, "Pressure", atmoPressure);

	Log_Debug("[Info] Sending telemetry %s\n", pjsonBuffer);
	AzureIoT_SendMessage(pjsonBuffer); // send the message
	free(pjsonBuffer); // clear the JSON buffer
}

int main(int argc, char *argv[])
{
	println("Universum");
	println("    Expanding Boundaries");
	println("");
	println("");

	// Variable to help us send the version string up only once
	println("[Setup] Setting Up Variables");
	println("[Setup] Success - Variables Setup complete");
	Log_Debug("[INFO] Version String: %s\n", argv[1]);

	if (InitPeripheralsAndHandlers() != 0) // setup 
	{
		terminationRequired = true;
	}

	println("[Setup] Success -  Setup Complete");
	println("");
	println("");
	buzz(1000);
	delay(3000);

	// this is the main loop in the code, it will repeat while the device is functioning correctly
	while (true)
	{
		ledWhite(); // turn the LED white
		if (getHeartRate()) // wait for button press and get the heart rate
		{
			println("");
			delay(1000);
			analyseData(); // analyse the data from the sample

			println("");
			delay(1000);
			getTemp();
			getAtmoPressure();
			delay(1000);

			println("");
			sendDataToAzure();
		}
		
		println("");
		println("");
		delay(2000);
	}
}