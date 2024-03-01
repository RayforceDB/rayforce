TARGET = rayforce

default:
	cd ../ && make CC="gcc" dll
	swig -c++ -javascript -node rayforce.i
	g++ -std=c++17 -fPIC -o rayforce.node -shared rayforce_wrap.cxx -I/usr/include/node/ -I$(node -p "require('node-addon-api').include") -L$(node -p "process.execPath") -I../core/ -lnode

clean:
	rm -rf build
	rm -rf *.c
	rm -rf *.cxx
