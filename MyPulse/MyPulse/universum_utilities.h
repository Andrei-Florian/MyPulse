/*
	universum_utilities
	13/OCT/2019 | Andrei Florian
*/

#pragma once

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
#include "mt3620_avnet_dev.h"

#include <applibs/log.h>

// utility to simply put the device to sleep for an amount of time
// timeToSleep - time to sleep in MILLISECONDS
void delay(float timeToSleep)
{
	const struct timespec sleepTime = { (timeToSleep / 1000), 0 }; // define the amount of time to sleep
	nanosleep(&sleepTime, NULL);
}


void println(char dataToPrint[])
{
	Log_Debug(dataToPrint);
	Log_Debug("\n");
}

void print(char dataToPrint[])
{
	Log_Debug(dataToPrint);
}