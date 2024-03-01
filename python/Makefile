TARGET = rayforce

default:
	cd ../ && make clean && make CC="gcc" dll
	swig -python rayforce.i
	gcc -fPIC -shared rayforce_wrap.c -o _rayforce.so -I../core -I/usr/include/python3.10 -lpython3.10 -L../ -lrayforce

clean:
	rm -rf __pycache__
	rm -f *.pyc
	rm -f *.so
	rm -f *.o
	rm -f *.c
	rm -f *.cxx
