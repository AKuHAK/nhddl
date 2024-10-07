// This code is a heavily modified version of OPL OSDHistory.c with unneeded bits removed
#include <errno.h>
#include <fcntl.h>
#include <libcdvd.h>
#include <ps2sdkapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "history.h"
#include "module_init.h"

// The 'X' in "BXDATA-SYSTEM" will be replaced with region-specific letter by initSystemDataDir
// The 'X' in "mcX" will be replaced with memory card number in updateHistoryFile
static char historyFilePath[] = "mcX:/BXDATA-SYSTEM/history";

static inline int initSystemDataDir(void);
void processHistoryList(const char *titleID, struct historyListEntry *historyList);
int evictEntry(const struct historyListEntry *evictedhistoryEntry);
static uint16_t getTimestamp(void);

// Adds title ID to the history file on both mc0 and mc1
int updateHistoryFile(const char *titleID) {
  // Detect system directory
  if (initSystemDataDir())
    return -ENOENT;

  // Try opening history file on mc0 and mc1
  int histfileFd, count;
  struct historyListEntry historyList[MAX_HISTORY_ENTRIES];
  for (char i = '0'; i < '2'; i++) { // Skipping int-char conversions thanks to ASCII code ordering
    historyFilePath[2] = i;
    histfileFd = open(historyFilePath, O_RDONLY);
    if (histfileFd < 0) { // File doesn't exist, continue
      continue;
    }

    printf("Updating history file at %s\n", historyFilePath);

    // Read history file
    count = read(histfileFd, historyList, HISTORY_FILE_SIZE);
    if (count != (HISTORY_FILE_SIZE)) {
      printf("Failed to load the history file, reinitializing\n");
      memset(historyList, 0, HISTORY_FILE_SIZE);
    }
    close(histfileFd);

    // Process history file
    processHistoryList(titleID, historyList);

    // Write history file
    histfileFd = open(historyFilePath, O_WRONLY | O_CREAT | O_TRUNC);
    if (histfileFd < 0) {
      printf("ERROR: Failed to open history file for writing: %d\n", histfileFd);
      continue;
    }
    // Return error if not all bytes were written
    count = write(histfileFd, historyList, HISTORY_FILE_SIZE);
    if (count != HISTORY_FILE_SIZE) {
      close(histfileFd);
      printf("ERROR: Failed to write: %d/%d bytes written\n", count, HISTORY_FILE_SIZE);
      continue;
    }
    close(histfileFd);
  }
  return 0;
}

// Reads ROM version from rom0:ROMVER and initializes historyFilePath with region-specific letter
static inline int initSystemDataDir(void) {
  int romver_fd = open("rom0:ROMVER", O_RDONLY);
  if (romver_fd < 0) {
    return -ENOENT;
  }

  char romver_str[5];
  read(romver_fd, romver_str, 5);
  close(romver_fd);

  switch (romver_str[4]) {
  case 'C': // China
    historyFilePath[6] = 'C';
    break;
  case 'E': // Europe
    historyFilePath[6] = 'E';
    break;
  case 'H': // Asia
  case 'A': // USA
    historyFilePath[6] = 'A';
    break;
  default: // Japan
    historyFilePath[6] = 'I';
  }

  return 0;
}

// Processes history record list, updating title entry if it already exists in the list
// or adding it to the list, evicting the least used title along the way
void processHistoryList(const char *titleID, struct historyListEntry *historyList) {
  // Used to find least used record
  int leastUsedRecordIdx = 0;
  int leastUsedRecordTimestamp = INT_MAX;
  int leastUsedRecordLaunchCount = INT_MAX;

  // Used to mark blank slots
  uint8_t blankSlots[MAX_HISTORY_ENTRIES];
  int blankSlotCount = 0;
  int i;
  // Loop over all histrory entries, trying to find the target title and least used entry
  for (i = 0; i < MAX_HISTORY_ENTRIES; i++) {
    // Check if this slot is used
    if (historyList[i].titleID[0] == '\0') {
      blankSlots[blankSlotCount] = i;
      blankSlotCount++;
      continue; // no point in continuing if the slot is empty
    }

    // Find least used entry
    if (historyList[i].launchCount < leastUsedRecordLaunchCount) {
      leastUsedRecordIdx = i;
      leastUsedRecordLaunchCount = historyList[i].launchCount;
    }
    // Find the oldest least used entry
    if (leastUsedRecordLaunchCount == historyList[i].launchCount) {
      if (historyList[i].timestamp < leastUsedRecordTimestamp) {
        leastUsedRecordTimestamp = historyList[i].timestamp;
        leastUsedRecordIdx = i;
      }
    }

    // Check if this entry belongs to the target title
    if (!strncmp(historyList[i].titleID, titleID, sizeof(historyList[i].titleID))) {
      printf("Updating entry at slot %d\n", i);
      // Update timestamp
      historyList[i].timestamp = getTimestamp();

      // Update launch count
      if ((historyList[i].bitmask & 0x3F) != 0x3F) {
        int newLaunchCount = historyList[i].launchCount + 1;
        if (newLaunchCount >= 0x80)
          newLaunchCount = 0x7F;

        if (newLaunchCount >= 14) {
          if ((newLaunchCount - 14) % 10 == 0) {
            int value;
            while ((historyList[i].bitmask >> (value = rand() % 6)) & 1) {
            };
            historyList[i].shiftAmount = value;
            historyList[i].bitmask |= 1 << value;
          }
        }
        historyList[i].launchCount = newLaunchCount;
      } else {
        if (historyList[i].launchCount < 0x3F) {
          historyList[i].launchCount++;
        } else {
          historyList[i].launchCount = historyList[i].bitmask & 0x3F;
          historyList[i].shiftAmount = 7;
        }
      }
      return;
    }
  }

  // If this title is not in the history file, add it
  struct historyListEntry *newEntry;
  int slot = 0;
  if (blankSlotCount > 0) {
    // Use random unused slot
    newEntry = &historyList[blankSlots[slot = rand() % blankSlotCount]];
  } else {
    // Copy out the victim record and evict it into history.old
    struct historyListEntry evictedhistoryEntry;
    newEntry = &historyList[slot = leastUsedRecordIdx];
    memcpy(&evictedhistoryEntry, newEntry, sizeof(evictedhistoryEntry));
    i = evictEntry(&evictedhistoryEntry);
    if (i < 0) // Will reuse i here for result
      printf("ERROR: Failed to append to history.old: %d\n", i);
  }

  printf("Inserting entry to slot %d\n", slot);
  // Initialize the new entry
  strncpy(newEntry->titleID, titleID, sizeof(newEntry->titleID) - 1);
  newEntry->launchCount = 1;
  newEntry->bitmask = 1;
  newEntry->shiftAmount = 0;
  newEntry->timestamp = getTimestamp();
}

// Appends evicted history entry to history.old file
int evictEntry(const struct historyListEntry *evictedhistoryEntry) {
  printf("Evicting %s into history.old\n", evictedhistoryEntry->titleID);
  char fullpath[64];
  int fd, result;

  strcpy(fullpath, historyFilePath);
  strcat(fullpath, ".old");
  if ((fd = open(fullpath, O_WRONLY | O_CREAT | O_APPEND)) >= 0) {
    lseek(fd, 0, SEEK_END);
    result = write(fd, evictedhistoryEntry, sizeof(struct historyListEntry)) == sizeof(struct historyListEntry) ? 0 : -EIO;
    close(fd);
  } else {
    result = fd;
  }
  return result;
}

// Returns timestamp suitable for history file entry
static uint16_t getTimestamp(void) {
  sceCdCLOCK time;
  sceCdReadClock(&time);
  return OSD_HISTORY_SET_DATE(btoi(time.year), btoi(time.month & 0x7F), btoi(time.day));
}