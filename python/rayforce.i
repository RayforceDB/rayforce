%module _rayforce

%{
#define SWIG_FILE_WITH_INIT
#include "../core/rayforce.h"
#include "../core/error.h"
#include <Python.h>

/* Function declarations */
extern obj_p eval_str(lit_p str);
extern obj_p i64(i64_t val);

/* Helper functions */
static const char* get_error_message(obj_p error_obj) {
    if (error_obj == NULL || error_obj->type != TYPE_ERROR) {
        return "Unknown error";
    }
    
    struct ray_error_t* err = (struct ray_error_t*)error_obj->arr;
    if (err == NULL) {
        return "Invalid error object";
    }
    
    obj_p msg = err->msg;
    if (msg == NULL || msg->type != TYPE_C8) {
        return "No error message available";
    }
    
    return msg->arr;
}

/* Wrapper functions */
static PyObject* _eval_str(PyObject *self, PyObject *args) {
    const char *str;
    obj_p result;
    PyObject *py_result = NULL;
    
    if (!PyArg_ParseTuple(args, "s", &str)) {
        PyErr_SetString(PyExc_TypeError, "Expected a string argument");
        return NULL;
    }
    
    /* Call eval_str() */
    result = eval_str(str);
    
    /* Convert result to Python object */
    if (result == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Evaluation failed: NULL result");
        return NULL;
    }
    
    switch (result->type) {
        case -TYPE_I64:
            py_result = PyLong_FromLongLong(result->i64);
            break;
        case TYPE_I64:
            py_result = PyLong_FromLongLong(result->i64);
            break;
        case TYPE_C8:
            py_result = PyUnicode_FromString(result->arr);
            break;
        case TYPE_ERROR:
            {
                const char* error_msg = get_error_message(result);
                PyErr_Format(PyExc_RuntimeError, "Evaluation error: %s", error_msg);
                drop_obj(result);
                return NULL;
            }
        default:
            PyErr_Format(PyExc_RuntimeError, "Unsupported result type: %d", result->type);
            drop_obj(result);
            return NULL;
    }
    
    /* Clean up */
    drop_obj(result);
    
    return py_result;
}

/* Method table */
static PyMethodDef methods[] = {
    {"eval_str", _eval_str, METH_VARARGS, "Evaluate a string expression"},
    {NULL, NULL, 0, NULL}
};

/* Module definition */
static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "_rayforce",
    NULL,
    -1,
    methods
};

/* Module initialization function */
PyMODINIT_FUNC PyInit__rayforce(void) {
    PyObject *m;
    
    /* Initialize rayforce runtime */
    if (ray_init() != 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to initialize rayforce runtime");
        return NULL;
    }
    
    m = PyModule_Create(&moduledef);
    if (m == NULL) {
        ray_clean();
        return NULL;
    }
    
    return m;
}
%}

// Better memory management
%feature("autodoc", "1");
%feature("ref")   obj_p "clone_obj($this);"
%feature("unref") obj_p "drop_obj($this);"

// Handle char* as Python strings
%include <typemaps.i>

/* Type definitions */
typedef char i8_t;
typedef char c8_t;
typedef char b8_t;
typedef char *str_p;
typedef const char *lit_p;
typedef unsigned char u8_t;
typedef void nil_t;
typedef struct obj_t *obj_p;

/* Handle nil_t as void */
%typemap(in) nil_t {
    /* No input needed for void */
}

%typemap(out) nil_t {
    /* No output needed for void */
    Py_INCREF(Py_None);
    $result = Py_None;
}

/* Function declarations */
extern obj_p eval_str(lit_p str);
extern obj_p i64(i64_t val);

/* Ignore internal implementation details */
%ignore obj_t::arr;

/* Include the header with our function declarations */
%include "../core/rayforce.h"
