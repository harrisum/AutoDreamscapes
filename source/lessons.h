#ifndef AUTODS_LESSONS_H
#define AUTODS_LESSONS_H

#include "desktop.h"

enum {
    LESSONS_ERROR = -1,
    LESSONS_MISSING = 0,
    LESSONS_OK = 1
};
typedef enum {
    LESSONS_ACTIVE,
    LESSONS_RECOVERED
} LessonFile;

typedef struct {
    uint64_t window;
    uint32_t process;
    int32_t width, height, top_label, count;
    Lesson items[MAX_LESSONS];
} SavedLessons;

int load_lessons(SavedLessons *map, LessonFile file);
int save_lessons(const SavedLessons *map, LessonFile file);

#endif
