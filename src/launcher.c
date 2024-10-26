#include "common.h"
#include "history.h"
#include "iso.h"
#include "options.h"
#include <elf-loader.h>
#include <ps2sdkapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <usbhdfsd-common.h>
// Required for retrieveing BDM driver name and index
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

static char isoArgument[] = "dvd";
static char bsdArgument[] = "bsd";

int getDeviceInfo(int mode, char *deviceInfo, size_t deviceInfoSize) {
  char path[16];
  snprintf(path, sizeof(path), "mass%d:/", mode);

  int dir = fileXioDopen(path);
  if (dir >= 0) {
    char driverName[16] = "Unknown"; // Default values in case of failure
    int deviceNumber = -1;

    // Retrieve the driver name
    if (fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, driverName,
                      sizeof(driverName) - 1) >= 0) {
      driverName[sizeof(driverName) - 1] = '\0'; // Ensure null termination
    }

    // Retrieve the device number
    fileXioIoctl2(dir, USBMASS_IOCTL_GET_DEVICE_NUMBER, NULL, 0, &deviceNumber,
                  sizeof(deviceNumber));

    // Reassign deviceInfo based on driverName content
    if (strcmp(driverName, "sdc") == 0)
      snprintf(deviceInfo, deviceInfoSize, "mx4sio");
    else if (strcmp(driverName, "sd") == 0)
      snprintf(deviceInfo, deviceInfoSize, "ilink");
    else if (strcmp(driverName, "udp") == 0)
      snprintf(deviceInfo, deviceInfoSize, "udpbd");
    else
      snprintf(deviceInfo, deviceInfoSize, "%s", driverName);

    fileXioDclose(dir); // Close the directory
    return deviceNumber;
  } else {
    snprintf(deviceInfo, deviceInfoSize, "Device not found");
    return -1;
  }
}

void extractDeviceMode(const char *fullPath, int *mode) {
  // Check if fullPath starts with "mass" and parse the following number
  if (strncmp(fullPath, "mass", 4) == 0) {
    *mode = fullPath[4] - '0';    // Convert character to integer
    if (*mode < 0 || *mode > 9) { // Ensure the mode is within a valid range
      *mode = 0;                  // Invalid mode if out of bounds
    }
  } else {
    *mode = 0; // Invalid mode if not a mass device path
  }
}

// Assembles argument lists into argv for Neutrino.
// Expects argv to be initialized with at least (arguments->total) elements.
int assembleArgv(ArgumentList *arguments, char **argv) {
  Argument *curArg = arguments->first;
  int argCount = 0;
  int argSize = 0;
  while (curArg != NULL) {
    if (!curArg->isDisabled) {
      if (!strlen(curArg->value) && !strcmp(COMPAT_MODES_ARG, curArg->arg)) {
        // Skip empty compatibility mode argument
        curArg = curArg->next;
        continue;
      }

      argSize = strlen(curArg->arg) + strlen(curArg->value) + 3; // + \0, = and -
      char *value = calloc(sizeof(char), argSize);

      if (!strlen(curArg->value))
        snprintf(value, argSize, "-%s", curArg->arg);
      else
        snprintf(value, argSize, "-%s=%s", curArg->arg, curArg->value);

      argv[argCount] = value;
      argCount++;
    }
    curArg = curArg->next;
  }

  // Free unused memory
  if (argCount != arguments->total)
    argv = realloc(argv, argCount * sizeof(char *));

  return argCount;
}

// Launches target, passing arguments to Neutrino.
// Expects arguments to be initialized
void launchTitle(Target *target, ArgumentList *arguments) {
  // Append arguments
  int massNr = 0;
  char deviceInfo[16];
  int devNr = 0;
  extractDeviceMode(target->fullPath, &massNr);
  devNr = getDeviceInfo(massNr, deviceInfo, sizeof(deviceInfo));

  appendArgument(arguments, newArgument(bsdArgument, deviceInfo));
  // we need to switch from our mass index to driver specific index
  target->fullPath[4] = devNr + '0';
  appendArgument(arguments, newArgument(isoArgument, target->fullPath));

  // Assemble argv
  char **argv = malloc((arguments->total) * sizeof(char *));
  int argCount = assembleArgv(arguments, argv);

  printf("Launching %s (%s) with arguments from path %s from device %s:\n", target->name, target->id, target->fullPath, deviceInfo);
  for (int i = 0; i < argCount; i++) {
    printf("%d: %s\n", i + 1, argv[i]);
  }

  if (updateLastLaunchedTitle(target->fullPath)) {
    printf("ERROR: Failed to update last launched title\n");
  }
  updateHistoryFile(target->id);

  printf("ERROR: failed to load %s: %d\n", NEUTRINO_ELF_PATH, LoadELFFromFile(NEUTRINO_ELF_PATH, argCount, argv));
}
