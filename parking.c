/*
 * AUTORES:
 *
 * Victor LOpez SAnchez
 * Mario LOpez PErez
*/



/* Macros */
#define _POSIX_C_SOURCE 200809L // Para sigaction
#define NUM_USER_SEM 1			// SemAforos para el usuario
#define NUM_USER_SHM 32			// Bytes de memoria comp. para el usuario

#include "parking.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/types.h>

/* Prototipos */
int limpiar_recursos(int sem_id, int shm_id, int buz_id);

void manejador_SIGINT(int sig);
void manejador_SIGALRM(int sig);

int llegada_primer_ajuste(HCoche hc);
int llegada_siguiente_ajuste(HCoche hc);
int llegada_mejor_ajuste(HCoche hc);
int llegada_peor_ajuste(HCoche hc);

void chofer();

/* Variables globales */
int sem_id = -1;
int shm_id = -1;
int buz_id = -1;
int nchof  = 0;
pid_t *pid = NULL;	// Array de pids hijos
pid_t pid_padre;

int main(int argc, char *argv[])
{
	pid_padre = getpid();

	/* Registro de seNales */
	// SIGINT
	struct sigaction sa;
	sa.sa_handler = manejador_SIGINT;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGINT, &sa, NULL) == -1) {
	    perror("Error al registrar SIGINT");
	    return 1;
	}
	// SIGALARM
	sa.sa_handler = manejador_SIGALRM;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        perror("Error al registrar SIGALRM");
        return 1;
    }

    TIPO_FUNCION_LLEGADA algoritmos_llegada[4]; // Creamos el array de funciones de llegada usando el typedef que nos da el .h

    /* Inicializo los elementos del array */
    algoritmos_llegada[0] = llegada_primer_ajuste;
    algoritmos_llegada[1] = llegada_siguiente_ajuste;
    algoritmos_llegada[2] = llegada_mejor_ajuste;
    algoritmos_llegada[3] = llegada_peor_ajuste;


    /* ComprobaciOn argumentos */
    if (argc == 1){
        puts("Llamada esperada: parking velocidad nchofers [D] [PA | PD]"); // Usamos puts y no pon_error porque aUn no hemos inicializado la biblioteca aquI
        return 1;
    } else if (argc < 3 || argc > 5){
        fprintf(stderr, "NUmero invAlido de parAmetros.\n");
        return 1;
    }

    /* Carga de los argumentos */
    int velocidad, D = 0, PA = 0, PD = 0;

    if (sscanf(argv[1], "%d", &velocidad) != 1 || velocidad < 0){
        fprintf(stderr, "Error: El primer argumento (velocidad) debe ser un entero >= 0.\n");
        return 1;	
	}
	if (sscanf(argv[2], "%d", &nchof) != 1 || nchof < 1) {
        fprintf(stderr, "Error: El segundo argumento (nchof) debe ser un entero > 0.\n");
        return 1;
    }

    /* Argumentos opcionales */
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

    /* CreaciOn del buzon */
    if ((buz_id = msgget(IPC_PRIVATE, IPC_CREAT | 0600)) == -1){	// Preguntar que permisos usar, 0060?
		perror("Error al crear el buzon.");
		kill(getpid(), SIGINT);
		return 1;
	}


    /* EjecuciOn */
    if (PARKING_inicio(velocidad, algoritmos_llegada, sem_id, buz_id, shm_id, D) == -1){
        perror("Error al ejecutar PARKING_inicio.");
        kill(getpid(), SIGINT);
		return 1;
    }

    
	/* CreaciOn procesos hijos (chOferes, cronOmetro) */
	pid = malloc((nchof+1)*sizeof(pid_t));

	for (int i=0; i<nchof+1; i++){
	    switch(pid[i]=fork()){
	        case -1:
	            perror("Error en fork de creaciOn cronOmetro");
	            kill(getpid(), SIGINT);	// Envia la señal SIGINT y salta a la manejadora
	            break;
	        case 0:
				// Hijos
				if (i == 0){
					write(STDOUT_FILENO, "\n[CRONÓMETRO] Iniciando cuenta atrás de 30 segundos...\n", 55);
                    alarm(30);	// Programa la señal SIGALRM para dentro de 30s
					pause();	// Espera a SIGALRM

				} else{
					// ChOferes
					chofer();
				}
				exit(0);
			default: 
			break;
	    }
	}
	
	// Solo el padre sale del for y llega aquI
	if (PARKING_simulaciOn() == -1){
		perror("Error al inicar la simulaciOn");
		kill(getpid(), SIGINT);
	}
	pause();	// Pause para pruebas


    /* LiberaciOn de recursos y finalizaciOn */
	kill(getpid(), SIGINT);
    return 0;
}





int limpiar_recursos(int sem_id, int shm_id, int buz_id){
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

	sem_id = -1;	
	buz_id = -1;	
	shm_id = -1;

    return cod_err;
}

/* Manejadoras */
void manejador_SIGINT(int sig) {
	if (getpid() == pid_padre){
		// Limpieza de recursos
    	write(STDOUT_FILENO, "\nLiberando recursos...\n", 23);
    	if (limpiar_recursos(sem_id, shm_id, buz_id)){
			write(STDOUT_FILENO, "No se han podido liberar los recursos.\n", 39);
			return;
    	} else
			write(STDOUT_FILENO, "Recursos liberados correctamente.\n", 34);

		// Limpieza de procesos
		if (pid != NULL){
            for (int i=0; i<nchof+1; i++){
                if (pid[i] > 0)
                    kill(pid[i], SIGTERM);
            }
            // Esperar a que terminen
            for (int i=0; i<nchof+1; i++){
                if (pid[i] > 0)
                    waitpid(pid[i], NULL, 0);
            }
            free(pid);
        }
		_exit(0);
	}
}

void manejador_SIGALRM(int sig){
    write(STDOUT_FILENO, "\n[CRONÓMETRO] Tiempo agotado. Finalizando simulación...\n", 56);
	PARKING_fin(1);
	// TODO: Esperar a que los que estAn desaparcando desaparquen
    kill(pid_padre, SIGINT);
}

/* Ajustes */
int llegada_primer_ajuste(HCoche hc){ // FunciOn de llegada de coche a la primera acera
    
    pon_error("Coche entrando en la primera acera\n");
    pause();
    return -1; // La cola estA ocupada de momento

}

int llegada_siguiente_ajuste(HCoche hc){ // FunciOn de llegada de coche a la segunda acera

    return -2; // Devolvemos -2 para que no moleste de momento en la ejecuciOn
}

int llegada_mejor_ajuste(HCoche hc){ // FunciOn de llegada de coche a la tercera acera

    return -2; // Devolvemos -2 para que no moleste de momento en la ejecuciOn
}

int llegada_peor_ajuste(HCoche hc){ // FunciOn de llegada de coche a la cuarta acera

    return -2; // Devolvemos -2 para que no moleste de momento en la ejecuciOn
}


void chofer(){
	pause();	// Para que no molesten mientras

	// TODO: Logica de los chOferes
	// Si quieres continua aqui

}