#include "stdio.h"
#include "stdlib.h"

/*** Seee mask.txt and example.cpp ****/

struct Field {
   char name[100];
   int x, y;
};

main() {
    unsigned char inputLine[500];
    Field field[100];
    int fieldCnt = 0;
    
    fprintf(stdout, "\n\n /**** Generated with support_programs/maskgenerator/generator.cpp ***/\n");
    fprintf(stdout, "\n\nconst char* screen[] = {\n");

    int ycnt=0;
    while(fgets((char*)inputLine, 500, stdin)) {
        ycnt++;
        bool inklammer = false;
	int charcnt =0;
        int xcnt = 0;
        for (int i=0; inputLine[i] != 0; i++) {
            xcnt++;
            if(inputLine[i] == 0xe2) xcnt -= 2; // multichar line character eg ┌────┐
            if(inputLine[i] == ']')  { inklammer = false; field[fieldCnt].name[charcnt] = 0; fieldCnt++; charcnt =0;}
            if(inklammer)            { field[fieldCnt].name[charcnt] = inputLine[i]; charcnt++; inputLine[i] = ' ';}
            if(inputLine[i] == '[')  { inklammer = true; field[fieldCnt].x = xcnt;field[fieldCnt].y = ycnt;}
            if(inputLine[i] == '\n' ){ inputLine[i] = 0;}
        }
        fprintf(stdout, "\"%s\",\n", inputLine);
    }
    fprintf(stdout, " 0 };\n\n\n");

    fprintf(stdout, "#define CLEAR_MASK \"\\x1B[2J\\x1B[1;1H\"\n");
    fprintf(stdout, "#define INIT_MASK() PRINTF(\"%%s\", CLEAR_MASK); for(int i = 0; screen[i] != 0; i++) PRINTF(\"%%s\\n\", screen[i]);\n\n");

    for(int i = 0; i < fieldCnt; i++) {
        fprintf(stdout, "#define %s  \"\\x1B[%d;%dH\"\n", field[i].name, field[i].y, field[i].x+1);
        //printf("%s (%d, %d)\n", field[i].name, field[i].x, field[i].y); 
    }
}