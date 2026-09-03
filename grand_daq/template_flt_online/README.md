# Module for Online Template-Fitting First-Level Trigger

This work is part of the NUTRIG project. It contains the scripts to integrate the template-fitting FLT-1 (or L2 trigger) to the GRAND DAQ.

- `main.cpp`: An example script that loads in the trace `test_trace.txt` and performs a template fit on it using the templates stored in `templates_96_XY_rfv2.txt`. 

- `template_flt.h`: This file defines the main class for the Template FLT-1.

- `error_handling.h`: This file defines the error handling that is used in the template fitting code.

- `utils.h`: This file defines some utils that are used in the template fitting code.