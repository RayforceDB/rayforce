TARGET = rayforce

default:
	cd ../ && make dll
	swig -c++ -javascript -node -o rayforce.c rayforce.i
	node-gyp configure
	node-gyp build

node:
	swig -c++ -javascript -node rayforce.i

clean:
	rm -rf build
	rm -rf *.c
	rm -rf *.cxx
