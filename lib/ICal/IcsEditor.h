#pragma once

#include <cstddef>
#include <cstdint>

#include "IcsDate.h"

// Streaming rewrites of an .ics file: the source is copied line by line to the
// sink, and only the targeted VTODO is buffered and changed, so files of any
// size are edited with a small, bounded amount of RAM. Unknown properties,
// folding and line order of untouched components are preserved verbatim.
namespace ics {

class Source {
 public:
  virtual ~Source() = default;
  // Returns bytes read, 0 at EOF, <0 on error.
  virtual int read(char* buf, int n) = 0;
};

class Sink {
 public:
  virtual ~Sink() = default;
  virtual bool write(const char* data, size_t n) = 0;
};

enum class TodoEdit : uint8_t { SetCompleted, SetIncomplete, Delete };

enum class EditStatus : uint8_t { Ok, NotFound, IoError, TooLarge };

// Applies `edit` to the VTODO whose UID equals `uid`. nowUtcStamp is
// "YYYYMMDDTHHMMSSZ" (see formatUtcStamp).
EditStatus editTodo(Source& src, Sink& out, const char* uid, TodoEdit edit, const char* nowUtcStamp);

struct NewTodo {
  const char* summary = nullptr;
  const char* uid = nullptr;
  int32_t dueDay = NO_DAY;  // all-day DUE when set
};

// Copies src (may be null for a new file) and inserts a VTODO before the
// final END:VCALENDAR, creating the calendar wrapper if needed.
EditStatus appendTodo(Source* src, Sink& out, const NewTodo& todo, const char* nowUtcStamp);

// Escapes TEXT per RFC 5545 (\\ \; \, \n) and folds the property line to 75
// octets without splitting UTF-8 sequences. Writes "NAME:escaped\r\n...".
bool writeTextProperty(Sink& out, const char* name, const char* text);

}  // namespace ics
