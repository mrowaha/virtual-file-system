all: lib format app writer reader deleter

lib: 	vsfs.c
	gcc -Wall -c vsfs.c
	ar -cvq libvsfs.a vsfs.o
	ranlib libvsfs.a

format: create_format.c
	gcc -Wall -o create_format  create_format.c   -L. -lvsfs

app: 	app.c
	gcc -Wall -o app app.c -L. -lvsfs

writer: writer.c
	gcc -Wall -o writer writer.c -L. -lvsfs

reader: reader.c
	gcc -Wall -o reader reader.c -L. -lvsfs

deleter: deleter.c
	gcc -Wall -o deleter deleter.c -L. -lvsfs

test:
	gcc -Wall vsfs.c vsfstest.c -o vsfstest -lcriterion

clean: 
	rm *.o libvsfs.a app vdisk create_format writer reader deleter

cleanall: 
	rm *.o libvsfs.a app vdisk create_format
