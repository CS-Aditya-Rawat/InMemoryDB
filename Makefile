server: server.o hashtable.o
		g++ server.o hashtable.o -o server	

server.o: server.cpp
		g++ -c server.cpp

hashmap.o: hashtable.cpp hashtable.h
		g++ -c hashtable.cpp

clean:
		rm *.o server
