/*
/////////////////////////////////////
//** ERROR HANDLING HEADER FILE ** //
/////////////////////////////////////

This file defines the error handling that is used in the template fitting code.
*/

#ifndef ERROR_HANDLING_H
#define ERROR_HANDLING_H

#include <string>

/*
---------
FUNCTIONS
---------
*/

void throwError(const std::string& message,
                const char* file,
                int line);

#endif // ERROR_HANDLING_H