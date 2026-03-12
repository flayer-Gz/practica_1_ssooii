#include "parking.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int main(int argc, char *argv[])
{
	/* Comprobacion argumentos */
	if (argc == 1){
		puts("Comando invAlido, ha de ser: parking velocidad nchofers [D] [PA | PD]");
		return 0;
	}
	else if (argc < 3 || argc > 5){
		perror("NUmero invAlido de parAmetros.");
		return 1;
	}

	/* Carga de los argumentos */
	int velocidad, nchof;
	if (sscanf(argv[1], "%d", &velocidad) != 1 || velocidad < 0){
		fprintf(stderr, "Error: El primer argumento (velocidad) debe ser un entero >= 0.\n");
		return 1;	
	}

	if (sscanf(argv[2], "%d", &nchof) != 1 || nchof < 1) {
       		 fprintf(stderr, "Error: El segundo argumento (choferes) debe ser un entero > 0.\n");
       		 return 1;
	    }

	/* Variables opcionales */
	int D = 0, PA = 0, PD = 0;
	for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "D") == 0) {
            D = 1;
        } else if (strcmp(argv[i], "PA") == 0) {
            PA = 1;
        } else if (strcmp(argv[i], "PD") == 0) {
            PD = 1;
        } else {
            fprintf(stderr,"Error: Argumento opcional '%s' no reconocido.\n", argv[i]);
            return 1;
        }
    }

	if (PA && PD) {
		fprintf(stderr, "Error: No se pueden usar PA y PD a la vez (son contradictorios).\n");
		return 1;
	}




	return 0;
}
