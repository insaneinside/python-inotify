#include <Python.h> 
#include <structmember.h>
#include <errno.h>
#include <string.h> 
#include <time.h>
#include <sys/types.h> 
#include <sys/inotify.h>
#include <sys/epoll.h>


/* module globals */
struct epoll_event *epoll_event = NULL; 
int inotify_fd = 0;
int epoll_fd = 0; 
int event_size = 0; 
void *inotify_buffer = NULL; 
int stoploop = 0; 
int restore_loop = 0;
PyObject *notify_callback = NULL; 


/* inotify event object */
struct inotify_event_object {
	PyObject_HEAD 
	PyObject *wd;
	PyObject *mask;
	PyObject *cookie;
	PyObject *length;
	PyObject *name; 
}; 



static PyTypeObject inotify_event_type; 
	
	

static int notify_client(struct inotify_event *ie)
{
	struct inotify_event_object *ieo;
	PyObject *cbargs;

	ieo = (struct inotify_event_object *)PyObject_New(
			struct inotify_event_object, &inotify_event_type
		);

	ieo->cookie = PyInt_FromLong(ie->cookie);
	ieo->wd = PyInt_FromLong(ie->wd); 
	ieo->mask = PyInt_FromLong(ie->mask); 
	ieo->length = PyInt_FromLong(ie->len);
	ieo->name = PyString_FromString(ie->name); 

	cbargs = Py_BuildValue("(O)", (PyObject *)ieo); 
	PyObject_CallObject(notify_callback, cbargs); 

	Py_DECREF(ieo);
	Py_DECREF(cbargs); 

	if (PyErr_Occurred()) {
		return -1;
	}

	return 0;

}


PyDoc_STRVAR(inotify_watch_doc,
		"add the file yout want to watch, this function accepts "
		"filepath, mask as parameters and  return a "
		"fd which you can use to identify yout file.)\n");


static PyObject *
inotify_watch(PyObject *object, PyObject *args)
{ 
	PyObject *mask;
	PyObject *name;
	int tmp = 0 ;
	if (!PyArg_ParseTuple(args, "OO:watch", &name, &mask))
		return NULL;

	if (!PyInt_Check(mask)) {
		PyErr_SetString(PyExc_TypeError,
				"mask should be a integer");
		return NULL;
	}

	if (!PyString_Check(name)) {
		PyErr_SetString(PyExc_TypeError, 
				"name should be a string");
		return NULL;
	} 

	if(inotify_fd == 0) {
		tmp = inotify_init1(IN_NONBLOCK);	
		if (tmp > 0) {
			inotify_fd = tmp;
		} else { 
			PyErr_SetString(PyExc_RuntimeError,
				"init inotify instance failed");
			return NULL;
		} 
	} 
	
	tmp = inotify_add_watch(inotify_fd, 
			PyString_AsString(name),
			PyInt_AsLong(mask));

	if(tmp < 0) { 
		PyErr_SetFromErrno(PyExc_OSError);
		return NULL; 
	}

	if (PyErr_Occurred()) {
		return NULL;
	}
	return PyInt_FromLong(tmp); 
}


PyDoc_STRVAR(inotify_unwatch_doc,
		"stop watching a file,  this function accepts a fd "
		"as parameter, which is returned by function watch.\n");


static PyObject *
inotify_unwatch(PyObject *object, PyObject *args)
{
	int tmp = 0;
	int wd = 0;
	
	if(!PyArg_ParseTuple(args, "i:unwatch", &wd))
		return NULL; 

	if(inotify_fd == 0) {
		PyErr_SetString(PyExc_OSError, 
				"bug report: inotify has not initialized yet"); 
	} 

	tmp = inotify_rm_watch(inotify_fd, wd);	
	if(tmp < 0) {
		PyErr_SetFromErrno(PyExc_OSError);
		return NULL;
	} 

	Py_RETURN_TRUE;
}


PyDoc_STRVAR(inotify_startloop_doc,
		"start the mainloop, this function accepts a callable "
		"object as parameter and return nothing, there is an "
		"optional parameter extra_code for you to add something "
		"to the mainloop, \n");


static PyObject *
inotify_startloop(PyObject *object, PyObject *args, PyObject *kwargs)
{
	int tmp = 0;
	int mfds = 0;
	int m = 0;
	int n = 0; 
	int block = 0;
	int timeout = 100; 
	PyObject *block_tmp = NULL; 
	PyObject *cb = NULL;
	PyObject *extra = NULL; 
	struct epoll_event ev; 

	static char *kwlist[] = {"callback", "extra", "timeout", "block", 0};

	if (PyArg_ParseTupleAndKeywords(args, kwargs, "O|OIO:startloop",
				kwlist, &cb, &extra, &timeout, 
				&block_tmp)) { 
		if (!PyCallable_Check(cb)) {
			PyErr_SetString(PyExc_TypeError,
					"extra should be a callable "
					"object");
			return NULL;
		} 
		if (extra != NULL) {
			if (!PyCallable_Check(extra)) {
				PyErr_SetString(PyExc_TypeError, 
						"extra_code should be "
						"a callable object");
				return NULL;
			}
		} 
		if (block_tmp) {
			if (!PyBool_Check(block_tmp)) {
				PyErr_SetString(PyExc_TypeError,
						"block should be a boolean");
			}
			block = PyInt_AsLong(block_tmp);
		} else {
			block = 1;
		}

		Py_XINCREF(cb);
		Py_XDECREF(notify_callback);
		notify_callback = cb;

	} else {
		return NULL;
	} 

	if(inotify_fd == 0) {
		tmp = inotify_init1(IN_NONBLOCK);	
		if (tmp > 0) {
			inotify_fd = tmp;
		}
		else { 
			PyErr_SetFromErrno(PyExc_OSError);
			return NULL; 
		} 
	} 
	ev.events = EPOLLIN;
	ev.data.fd = inotify_fd;

	if(epoll_fd == 0) {
		tmp = epoll_create(1); 
		if (tmp > 0) { 
			epoll_fd = tmp;
		}
		else { 
			PyErr_SetFromErrno(PyExc_OSError);
			return NULL; 
		} 
	} 
	if (!restore_loop) { 
		tmp = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, inotify_fd, &ev);
		if(tmp < 0) {
			PyErr_SetFromErrno(PyExc_OSError);
			return NULL; 
		} 
		stoploop = 1;
	}

	while(1) {
		if (stoploop == 0) { 
			Py_RETURN_NONE;
		}
		mfds = epoll_wait(epoll_fd, epoll_event, 100, timeout);
		if (mfds < 0) {
			PyErr_SetFromErrno(PyExc_OSError);
			return NULL;
		}
		for (m=0; m < mfds; ++m) {
			n = read(inotify_fd, inotify_buffer, event_size);
			if (errno == EAGAIN)
				continue;
			if (n < 0) {
				PyErr_SetFromErrno(PyExc_OSError);
				return NULL;
			}
			if (notify_client((struct inotify_event *)\
						inotify_buffer) < 0\
					) {
				return NULL;
			}	
		} 
		if (extra) { 
			PyObject_CallObject(extra, NULL); 
		} 
		if (!block) {
			restore_loop = 1;
			break;
		}
		m = 0; 
	}
	Py_RETURN_NONE;
	
}


PyDoc_STRVAR(inotify_stoploop_doc, 
		"stop the main loop\n");
	    
		
static PyObject *
inotify_stoploop(PyObject *object, PyObject *args)
{
	stoploop = 0;
	close(epoll_fd);
	close(inotify_fd);
	epoll_fd = 0;
	inotify_fd = 0; 
	restore_loop = 0;
	Py_RETURN_NONE;
}


static PyMethodDef inotify_methods[] = { 
	{"startloop", (PyCFunction)inotify_startloop,
		METH_VARARGS|METH_KEYWORDS, inotify_startloop_doc}, 
	{"stoploop", (PyCFunction)inotify_stoploop,
		METH_NOARGS, inotify_stoploop_doc},
	{"watch", (PyCFunction)inotify_watch,
		METH_VARARGS, inotify_watch_doc},
	{"unwatch", (PyCFunction)inotify_unwatch,
		METH_VARARGS, inotify_unwatch_doc},
	{NULL, NULL, 0, NULL}
};


static PyMemberDef inotify_event_members[] = { 
	{"cookie", T_OBJECT, offsetof(struct inotify_event_object, cookie),
		READONLY, "event signature"}, 
	{"length", T_OBJECT, offsetof(struct inotify_event_object, length),
		READONLY, "name length"},
	{"wd", T_OBJECT, offsetof(struct inotify_event_object, wd),
		READONLY, "watch descriptor"},
	{"mask", T_OBJECT, offsetof(struct inotify_event_object, mask),
		READONLY, "event mask"},
	{"name", T_OBJECT, offsetof(struct inotify_event_object, name),
		READONLY, "optional name"},
	{NULL}
};


static void 
inotify_event_dealloc(struct inotify_event_object* self)
{ 
	Py_CLEAR(self->cookie);
	Py_CLEAR(self->length); 
	Py_CLEAR(self->wd);
	Py_CLEAR(self->mask);
	Py_CLEAR(self->name);
	PyObject_Del(self);
}

static PyObject* 
inotify_event_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	struct inotify_event_object *self;		
	self = PyObject_New(struct inotify_event_object, type);

	if (!self)
		return NULL;

	self->wd = PyInt_FromLong(0);	

	if (!self->wd) {
		Py_DECREF(self);
		return NULL;
	}
	self->mask = PyInt_FromLong(0);
	if (!self->mask) {
		Py_DECREF(self);
		return NULL;
	}
	self->cookie = PyInt_FromLong(0);		
	if (!self->cookie) {
		Py_DECREF(self);
		return NULL;
	}
	self->length = PyInt_FromLong(0);
	if (!self->length) {
		Py_DECREF(self);
		return NULL;
	}
	self->name = PyString_FromString("");
	if (!self->name) {
		Py_DECREF(self);
		return NULL;
	} 

	return (PyObject *)self;
}


PyDoc_STRVAR(event_doc, 
		"document: man inotify -> struct inotify_event"); 
		


static PyTypeObject inotify_event_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"inotify.event",
	sizeof(struct inotify_event_object),
	0,
	/* methods */
	(destructor)inotify_event_dealloc,	/*tp_dealloc*/
	0,			/*tp_print*/
	0,			/*tp_getattr*/
	0,			/*tp_setattr*/
	0,			/*tp_compare*/
	0,			/*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	0,			/*tp_as_mapping*/
	0,			/*tp_hash*/
	0,			/*tp_call*/
	0,			/*tp_str*/
	0,			/*tp_getattro*/
	0,			/*tp_setattro*/
	0,			/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT,	/*tp_flags*/
	event_doc,		/*tp_doc*/
	0,			/*tp_traverse*/
	0,			/*tp_clear*/
	0,			/*tp_richcompare*/
	0,			/*tp_weaklistoffset*/
	0,			/*tp_iter*/
	0,			/*tp_iternext*/
	0,			/*tp_methods*/
	inotify_event_members,	/*tp_members*/
	0,			/*tp_getset*/
	0,			/*tp_base*/
	0,			/*tp_dict*/
	0,			/*tp_descr_get*/
	0,			/*tp_descr_set*/
	0,			/*tp_dictoffset*/
	0,			/*tp_init*/
	0,			/*tp_alloc*/
	inotify_event_new,	/*tp_new*/
};



PyMODINIT_FUNC initinotify(void)
{ 
	PyObject *m; 
	Py_TYPE(&inotify_event_type) = &PyType_Type;
	if (PyType_Ready(&inotify_event_type) < 0)
		return;
	m  = Py_InitModule("inotify", inotify_methods);
	if (m == NULL)
		return; 
	Py_INCREF((PyObject *) &inotify_event_type);
	PyModule_AddObject(m, "event", (PyObject *) &inotify_event_type);
	/*inotify consts*/
	PyModule_AddObject(m, "IN_ACCESS", PyInt_FromLong(IN_ACCESS));
	PyModule_AddObject(m, "IN_ATTRIB", PyInt_FromLong(IN_ATTRIB));
	PyModule_AddObject(m, "IN_CREATE", PyInt_FromLong(IN_CREATE));
	PyModule_AddObject(m, "IN_DELETE", PyInt_FromLong(IN_DELETE));
	PyModule_AddObject(m, "IN_MOVE", PyInt_FromLong(IN_MOVE)); 
	PyModule_AddObject(m, "IN_MODIFY", PyInt_FromLong(IN_MODIFY));
	PyModule_AddObject(m, "IN_MOVE_SELF", PyInt_FromLong(IN_MOVE_SELF));
	PyModule_AddObject(m, "IN_MOVED_TO", PyInt_FromLong(IN_MOVED_TO));
	PyModule_AddObject(m, "IN_OPEN", PyInt_FromLong(IN_OPEN));
	PyModule_AddObject(m, "IN_MASK_ADD", PyInt_FromLong(IN_MASK_ADD));
	PyModule_AddObject(m, "IN_ONESHOT", PyInt_FromLong(IN_ONESHOT));
	PyModule_AddObject(m, "IN_ONLYDIR", PyInt_FromLong(IN_ONLYDIR));
	PyModule_AddObject(m, "IN_IGNORED", PyInt_FromLong(IN_IGNORED));
	PyModule_AddObject(m, "IN_ISDIR", PyInt_FromLong(IN_ISDIR));
	PyModule_AddObject(m, "IN_UNMOUNT", PyInt_FromLong(IN_UNMOUNT)); 
	PyModule_AddObject(m, "IN_CLOSE", PyInt_FromLong(IN_CLOSE));
	PyModule_AddObject(m, "IN_CLOSE_WRITE",
			PyInt_FromLong(IN_CLOSE_WRITE));
	PyModule_AddObject(m, "IN_CLOSE_NOWRITE",
			PyInt_FromLong(IN_CLOSE_NOWRITE)); 
	PyModule_AddObject(m, "IN_DELETE_SELF",
			PyInt_FromLong(IN_DELETE_SELF));
	PyModule_AddObject(m, "IN_MOVED_FROM",
			PyInt_FromLong(IN_MOVED_FROM));
	PyModule_AddObject(m, "IN_ALL_EVENTS", 
			PyInt_FromLong(IN_ALL_EVENTS)); 
	PyModule_AddObject(m, "IN_DONT_FOLLOW",
			PyInt_FromLong(IN_DONT_FOLLOW));
	PyModule_AddObject(m, "IN_EXCL_UNLINK",
			PyInt_FromLong(IN_EXCL_UNLINK));
	PyModule_AddObject(m, "IN_Q_OVERFLOW",
			PyInt_FromLong(IN_Q_OVERFLOW)); 

	/*module globals*/
	event_size = sizeof(struct inotify_event) + NAME_MAX + 1; 
	inotify_buffer = PyMem_Malloc(sizeof(char) * event_size); 
	epoll_event = PyMem_Malloc(sizeof(struct epoll_event) * 10);
}
