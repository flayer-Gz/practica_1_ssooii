/*
 * AUTORES:
 *
 * Victor LOpez SAnchez
 * Mario LOpez PErez
 */


#include "parking.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <signal.h>

/* Macros */
#define USER_SEM 1
#define USER_SHM 4




int main(int argc, char *argv[])
{

      




      


    /* Comprobacion argumentos */
    if (argc == 1){
        puts("Llamada esperada: parking velocidad nchofers [D] [PA | PD]");
        return 1;
    } else if (argc < 3 || argc > 5){
        fprintf(stderr, "NUmero invAlido de parAmetros.\n");
        return 1;
    }

    /* Carga de los argumentos */
    int velocidad, nchof, D = 0, PA = 0, PD = 0;

    if (sscanf(argv[1], "%d", &velocidad) != 1 || velocidad < 0){
        fprintf(stderr, "Error: El primer argumento (velocidad) debe ser un entero >= 0.\n");
        return 1;	
	}
	if (sscanf(argv[2], "%d", &nchof) != 1 || nchof < 1) {
        fprintf(stderr, "Error: El segundo argumento (nchof) debe ser un entero > 0.\n");
        return 1;
    }

    // Argumentos opcionales
	for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "D") == 0){
            D = 1;
        } else if (strcmp(argv[i], "PA") == 0){
            PA = 1;
        } else if (strcmp(argv[i], "PD") == 0){
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

    /* CreaciOn de los semAforos */
    int sem_id = semget(IPC_PRIVATE, PARKING_getNSemAforos() + USER_SEM, IPC_CREAT | 0600);

    /* CreaciOn de la memoria compartida */
    int shm_id = shmget(IPC_PRIVATE, PARKING_getTamaNoMemoriaCompartida() + USER_SHM, IPC_CREAT | 0600);

    /* CreaciOn de los buzones */
    int buz_id = msgget(IPC_PRIVATE, IPC_CREAT | 0600); // Posibles permisos: 0444
                                                        // Preguntar que permisos usar, 0060?


    /* Ejecucion */
    int debug;


    if (PARKING_inicio(velocidad, NULL, sem_id, buz_id, shm_id, debug) == -1){
        perror("Error al iniciar el parking, tontito");
        // Linpiar los recursos y acabar
    }











    /* LiberaciOn de recursos */
    









/*
    if (limpiarRecursos(sem_id, shmid, buz_id)){
        return 1;
    }
    */
	    return 0;
}


/*

int limpiarRecursos(int sem_id, int shm_id, int buz_id){
    int cod_err=0;

    if (semctl(sem_id, 0, IPC_RMID) == -1){
        perror("Error al liberar recurso: semaforos");
        cod_err = 1;
    }
    if (msgctl(buz_id, IPC_RMID, ) == -1){
        perror("Error al liberar recurso: buzon");
        cod_err = 1;
    }
    if (shmctl(shm_id, IPC_RMID, ) == -1){
        perror("Error al liberar recurso: memoria compartida");
        cod_err = 1;
    }

    return cod_err;
}

*/
