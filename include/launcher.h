#ifndef _LAUNCHER_H_
#define _LAUNCHER_H_

#include "options.h"
#include "iso.h"

// Launches target, passing arguments to Neutrino.
// Expects arguments to be initialized
void launchTitle(struct Target *target, struct ArgumentList *arguments);

#endif