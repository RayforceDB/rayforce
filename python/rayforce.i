%module rayforce
%{
#include "rayforce.h"
%}

%ignore obj_t::arr; // Ignore the flexible array member
%include "../core/rayforce.h"
