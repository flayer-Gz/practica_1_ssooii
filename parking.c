/*
 * AUTORES:
 *
 * Victor LOpez SAnchez
 * Mario LOpez PErez
*/

/* Includes */
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


/* Recursos compartidos*/
typedef struct {
    int aceras[4][80]; // Las 4 aceras cada una con 80 espacios (0=libre, 1=ocupado)
    // AquI turno chOferes
} DatosCompartidos;


/* Macros */
#define _POSIX_C_SOURCE 200809L // Para sigaction
#define NUM_USER_SEM 1			// SemAforos para el usuario
#define NUM_USER_SHM (sizeof(DatosCompartidos) + 8)		// Bytes de memoria comp. para el usuario (con un poco de mArgen por si acaso)



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



DatosCompartidos *memoria_compartida = NULL; // Puntero a todos los datos de la memoria compartida

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

    /* Uso de shmat para enganchar*/
    void *memoria_base = shmat(shm_id, NULL, 0); // Puntero genErico
    if (memoria_base == (void *)-1) { // Cast para que no de error la comparaciOn
        perror("Error al enganchar la memoria compartida (shmat)");
        kill(getpid(), SIGINT);
        return 1;
    }

    int offset = PARKING_getTamaNoMemoriaCompartida();

    /* Offset para asegurar que Encina no da Bus Error */
    if (offset % 4 != 0) {
        offset = offset + (4 - (offset % 4));
    }

    /* Ajustamos la posiciOn dOnde comienza nuestra memoria compartida */
    memoria_compartida = (DatosCompartidos *)((char *)memoria_base + offset);

    /* Inicializamos las 4 aceras a 0 (libre) */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 80; j++) {
            memoria_compartida->aceras[i][j] = 0;
        }
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
    int tamano_coche = PARKING_getLongitud(hc); // Averigua cuAnto mide el coche
    int huecos_consecutivos = 0; // Variable auxiliar para contar los huecos seguidos que encontramos en la zona de aparcamiento

    for(int i = 0; i<80; i++){
        if(memoria_compartida->aceras[0][i] == 0){ // Si el hueco estA vacIo, incremento el contador
            
            huecos_consecutivos++;
            // Si el hueco es suficientemente grande como para que quepa el coche, devuelvo esa posiciOn para que el chOfer sepa dOnde aparcar
            if(tamano_coche == huecos_consecutivos){
                int posiciOn_aparcamiento = i - huecos_consecutivos + 1; // Devolvemos la primera posiciOn del hueco libre del aparcamiento

                // Bucle para reservar esa posiciOn antes de que nos la quite otro coche
                for(int j = posiciOn_aparcamiento; j<=i; j++){ 
                    memoria_compartida->aceras[0][j] = 1; // Ponemos a 1 (ocupado) todos esos huecos que antes estaban a 0 (libre)
                }
                return posiciOn_aparcamiento;
        }
        }else{
            // Si el hueco no estA vacIo (tendrA un 1), reinicio el contador de huecos
            huecos_consecutivos = 0;
        }
    }

    // El bucle a acabado y no ha econtrado hueco para ese coche, no puede aparcar aUn
    return -1; // Todos los sitios ocupados/no suficientemente grandes

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
	struct PARKING_mensajeBiblioteca msg;

    while(42){
		// sizeof(msg) - sizeof(long) porque el campo tipo no cuenta para el mensaje
        if (msgrcv(buz_id, &msg, sizeof(msg) - sizeof(long), PARKING_MSG, 0) == -1){
            perror("[CHOFER] Error al leer del buzón");
            break;
        }

        if (msg.subtipo == PARKING_MSGSUB_APARCAR){
            pon_error("[CHOFER] Coche solicita APARCAR\n");
        } else if (msg.subtipo == PARKING_MSGSUB_DESAPARCAR){
            pon_error("[CHOFER] Coche solicita DESAPARCAR\n");
        }
    }
}