/////////////////////////////////////
//** ERROR HANDLING SOURCE FILE ** //
/////////////////////////////////////

#include "error_handling.h"
#include <sstream>
#include <iostream>

using namespace std;

/*
Throws an error including a reference to the file and line number where the error occured.
Credit: ChatGPT.

Arguments
---------
`message` : The error message to throw.

`file` : The file where the error was called.

`line` : The line number where the error was called.
*/
void throwError(const string& message,
                const char* file,
                int line) {
    ostringstream oss;
    oss << "Error in file " << file << " at line " << line << ": " << message;
    throw runtime_error( oss.str() );
}