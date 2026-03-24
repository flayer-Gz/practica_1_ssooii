/*
 * AUTORES:
 *
 * Victor LOpez SAnchez
 * Mario LOpez PErez
 */

 /* Macros */
#define _POSIX_C_SOURCE 200809L // Para sigaction
#define NUM_USER_SEM 1  // SemAforos para el usuario
#define NUM_USER_SHM 4  // Bytes de memoria comp. para el usuario

#include "parking.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <signal.h>
#include <unistd.h>

// IPC IDs
int sem_id = -1;
int shm_id = -1;
int buz_id = -1;

/* Prototipos */
int limpiarRecursos(int sem_id, int shm_id, int buz_id);
void manejadorSIGINT(int sig);

int main(int argc, char *argv[])
{

    struct sigaction sa;
	sa.sa_handler = manejadorSIGINT;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGINT, &sa, NULL) == -1) {
	    perror("Error al registrar SIGINT");
	    return 1;
	}



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
    if ((sem_id = semget(IPC_PRIVATE, PARKING_getNSemAforos() + NUM_USER_SEM, IPC_CREAT | 0600)) == -1){
		perror("Error al crear los semAforos.");
		return 1;
	}

    /* CreaciOn de la memoria compartida */
    if ((shm_id = shmget(IPC_PRIVATE, PARKING_getTamaNoMemoriaCompartida() + NUM_USER_SHM, IPC_CREAT | 0600)) == -1){
		perror("Error al reservar la memoria compartida.");
		kill(getpid(), SIGINT);
		return 1;
	}

    /* CreaciOn de los buzones */
    if ((buz_id = msgget(IPC_PRIVATE, IPC_CREAT | 0600)) == -1){	// Preguntar que permisos usar, 0060?
		perror("Error al crear el buzon.");
		kill(getpid(), SIGINT);
		return 1;
	}


    /* Ejecucion */
    int debug=0;	// debug = D ???

    if (PARKING_inicio(velocidad, NULL, sem_id, buz_id, shm_id, debug) == -1){
        perror("Error al ejecutar PARKING_inicio.");
        kill(getpid(), SIGINT);
		return 1;
    }

	// TODO: Crear procesos hijos (chOferes)


	// TODO: Llamada a PARKING_simulaciOn()



	sleep(30);



    /* LiberaciOn de recursos y finalizaciOn */
	kill(getpid(), SIGINT);
    return 0;
}





int limpiarRecursos(int sem_id, int shm_id, int buz_id){
    int cod_err=0;

    if ((sem_id != -1) && (semctl(sem_id, 0, IPC_RMID) == -1)){
        perror("Error al liberar recurso: semaforos");
        cod_err = 1;
    }
    if ((buz_id != -1) && (msgctl(buz_id, IPC_RMID, NULL) == -1)){	// IPC_RMID no utiliza el bufer -> se le puede pasar NULL
        perror("Error al liberar recurso: buzon");
        cod_err = 1;
    }
    if ((shm_id != -1) && (shmctl(shm_id, IPC_RMID, NULL) == -1)){
        perror("Error al liberar recurso: memoria compartida");
        cod_err = 1;
    }

    return cod_err;
}

void manejadorSIGINT(int sig) {
    write(STDOUT_FILENO, "\nLiberando recursos...\n", 23);
    if (limpiarRecursos(sem_id, shm_id, buz_id)){
		write(STDOUT_FILENO, "No se han podido liberar los recursos.\n", 39);
		return;
    } else
		write(STDOUT_FILENO, "Recursos liberados correctamente.\n", 34);

	_exit(1);	// Deja a los hijos huerfanos
}
