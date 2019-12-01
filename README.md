# Overview
Healthcare is rapidly developing every day. With an increased number of patients needing care around the world, hospitals have to be on top of their game and people aware of their health. But one should not be strained to visiting a doctor to get an SpO2 test as this would crowd hospitals.

The problem is that with technology advancing, healthcare must keep the pace and enter the age of IoT. Hospitals have taken great leaps to digitalise their records and recipes but what happens when the patient leaves the hospital with a disease such as COPD? How does the doctor know if the patient is recovering without the need of constant visits?

Introducing MyPulse, a smart IoT device that can sample the user’s heart rate and SpO2 (blood oxygen) levels in 30 seconds. MyPulse is a device that can be placed anywhere in a house or a hospital. All the user has to do to get their pulse is place their finger on the sensor.

# Benefits
1.	A device can be placed in the waiting room at the hospital allowing the patients to get their heart rate and SpO2 levels before going into the check-up making the process faster.
2.	A device can be given to the patients to bring home and monitor their vitals. The device can alert the patients if they need to see a doctor.
3.	The doctor will be able to remotely monitor the vitals of his/her patients without the need of a check-up lowering the doctor’s workload.
4.	The data is illustrated on a dashboard allowing the doctor and/or the patient to visualise the vitals.
5.	The data is stored in a scalable server allowing an array of devices to be used and distributed.

# Project Architecture
1.	The device is turned on when the button is pressed.
2.	The device then samples the vitals from the sensor.
3.	The device will then check if the sample is successful.
4.	If the sample is successful, the device will take the temperature and atmospheric pressure samples and send the data to the backend.
5.	The data is then received by Azure IoT Hub and queried by the stream analytics job.
6.	The data is finally outputted to Power Bi and displayed on a dashboard.
