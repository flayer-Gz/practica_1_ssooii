/*
 * AUTORES:
 *
 * Victor LOpez SAnchez
 * Mario LOpez PErez
*/

#define _POSIX_C_SOURCE 200809L // Para sigaction

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

union semun {
    int val;                /* Valor para SETVAL */
    struct semid_ds *buf;   /* Buffer para IPC_STAT, IPC_SET */
    unsigned short *array;  /* Arreglo para GETALL, SETALL */
};

typedef struct {
	pid_t pid;
    int x, y;
    int x_futuro, y_futuro;
    int longitud;
    int esperando_a; // -1 si no espera a nadie, o el ID del chofer que le bloquea
    int necesita_liberar; // Flag que representa si un coche debe liberar su hueco de parking o no
	int mi_turno;
} InfoChofer;

/* Recursos compartidos*/
typedef struct {
	int aceras[4][80];		// Las 4 aceras cada una con 80 espacios (0=libre, 1=ocupado)
	int turno_aparcar;		// Siguiente coche que le coche que le toca aparcar
	int turno_dispensador;	// Siguiente turno que da la biblioteca
} DatosCompartidos;


/* Macros */
#define NUM_USER_SHM (sizeof(DatosCompartidos) + 8)		// Bytes de memoria comp. para el usuario (con un poco de mArgen por si acaso)
#define NUM_USER_SEM 3									// SemAforos para el usuario
#define USER_SEM_1 PARKING_getNSemAforos()+nchof		// Despues de los semaforos de la biblioteca y los choferes
#define USER_SEM_2 PARKING_getNSemAforos()+nchof+1
#define CHOF_SEM PARKING_getNSemAforos()+nchof+2


/* Prototipos */
int limpiar_recursos();

void manejador_SIGINT(int sig);
void manejador_SIGALRM(int sig);

int llegada_primer_ajuste(HCoche hc);
int llegada_siguiente_ajuste(HCoche hc);
int llegada_mejor_ajuste(HCoche hc);
int llegada_peor_ajuste(HCoche hc);

void chofer(int n);

void aparcar_commit(HCoche hc);
void permiso_avance(HCoche hc);
void permiso_avance_commit(HCoche hc);

void wait_sem(int num_semAforo);
void signal_sem(int num_semAforo);

/* Variables globales */
int sem_id = -1;
int shm_id = -1;
int buz_id = -1;
int nchof  = 0;
pid_t pid_padre;
pid_t *pid = NULL;	// Array de pids hijos
InfoChofer *chof = NULL; // Puntero al array dinámico en memoria compartida
int chof_id = -1;	// Creo que esto no hace falta que sea global, puede ser local para cada chofer



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

    // CAlculo del tamaNo total: Estructura + array de chOferes
    size_t tamano_total = sizeof(DatosCompartidos) + (nchof * sizeof(InfoChofer));

    /* CreaciOn de recursos IPCs */
    if ((sem_id = semget(IPC_PRIVATE, PARKING_getNSemAforos() + NUM_USER_SEM + nchof, IPC_CREAT | 0600)) == -1){
		perror("Error al crear los semAforos.");
		return 1;
	}
    if ((shm_id = shmget(IPC_PRIVATE, PARKING_getTamaNoMemoriaCompartida() + tamano_total, IPC_CREAT | 0600)) == -1){ // QuizAs hay que poner el offset aqui
		perror("Error al reservar la memoria compartida.");
		kill(getpid(), SIGINT);
		return 1;
	}
	if ((buz_id = msgget(IPC_PRIVATE, IPC_CREAT | 0600)) == -1){	// Preguntar que permisos usar, 0060?
		perror("Error al crear el buzon.");
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
	
    /* Offset para asegurar que Encina no da Bus Error */
    int offset = PARKING_getTamaNoMemoriaCompartida();
    if (offset % 4 != 0) {
        offset = offset + (4 - (offset % 4));
    }

    /* Ajustamos la posiciOn dOnde comienza nuestra memoria compartida */
    memoria_compartida = (DatosCompartidos *)((char *)memoria_base + offset);
	
	/* Inicializamos los turnos */
	memoria_compartida->turno_aparcar = 0;
	memoria_compartida->turno_dispensador = 0;

    // El array de la posiciOn de los chOferes empieza justo donde acaba la estructura
    chof = (InfoChofer *)(memoria_compartida + 1);

    /* Inicializamos las 4 aceras a 0 (libre) */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 80; j++) {
    	    memoria_compartida->aceras[i][j] = 0;
        }
    }

    /* Mandamos todos los choferes a -1 antes de empezar por si durante la ejecuciOn con 4 chOferes por ejemplo sOlo estAn 2 antendiendo que no bloqueen al quedarse en la (x,y)=0,0*/
    for(int i = 0; i < nchof; i++){
        chof[i].x = -1;
        chof[i].y = -1; 
        chof[i].x_futuro = -1;
        chof[i].y_futuro = -1;
        chof[i].esperando_a = -1;
        chof[i].necesita_liberar = 0;
        chof[i].longitud = 0;
    }

	// Inicializamos todos los semAforos de los chOferes a 0
    union semun arg;
    arg.val = 0;
    for (int i = 0; i < nchof; i++) {
        semctl(sem_id, PARKING_getNSemAforos() + i, SETVAL, arg); 
    }

	// Inicializar el semAforo de usuario a 1 (como un Mutex)
	arg.val=1;
	if (semctl(sem_id, USER_SEM_1, SETVAL, arg) == -1) {
	    perror("Error inicializando semAforo");
		kill(getpid(), SIGINT);
        return 1;
	}
	// Inicializar el semAforo de usuario a 1 (como un Mutex)
	if (semctl(sem_id, USER_SEM_2, SETVAL, arg) == -1) {
	    perror("Error inicializando semAforo");
		kill(getpid(), SIGINT);
        return 1;
	}
	if (semctl(sem_id, CHOF_SEM, SETVAL, arg) == -1) {
	    perror("Error inicializando semAforo");
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
					// write(STDOUT_FILENO, "\n[CRONÓMETRO] Iniciando cuenta atrás de 30 segundos...\n", 55);
                    alarm(30);	// Programa la señal SIGALRM para dentro de 30s
                    // write(STDOUT_FILENO, "\nAlarma activada\n", 17);
					pause();	// Espera a SIGALRM
                    // write(STDOUT_FILENO, "\nSi escribe esto no sabemos nada de C\n", 38);
				} else{
					// ChOferes
					chofer(i-1);	// -1 para que estEn numerados desde el 0
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


    /* LiberaciOn de recursos y finalizaciOn */
	kill(getpid(), SIGINT);
    return 0;
}





int limpiar_recursos(){
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
    	if (limpiar_recursos()){
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
	PARKING_fin(1);
    _exit(0);
}


void chofer(int n){
	struct PARKING_mensajeBiblioteca msg;
	chof_id = n;

	while(42){	// TODO: Cambiar por espera sin consumo de CPU
		wait_sem(CHOF_SEM);
		if (msgrcv(buz_id, &msg, sizeof(msg) - sizeof(long), PARKING_MSG, 0) == -1){	// sizeof(msg) - sizeof(long) porque el campo tipo no cuenta para el mensaje
			perror("[CHOFER] Error al leer del buzón");
			break;
		}
		chof[chof_id].mi_turno = memoria_compartida->turno_dispensador;
		memoria_compartida->turno_dispensador++;
		
		wait_sem(USER_SEM_1);
		chof[chof_id].longitud = PARKING_getLongitud(msg.hCoche);
		chof[chof_id].x = PARKING_getX(msg.hCoche);
		chof[chof_id].y = PARKING_getY(msg.hCoche);
		signal_sem(USER_SEM_1);
		signal_sem(CHOF_SEM);
		if (msg.subtipo == PARKING_MSGSUB_APARCAR){
			wait_sem(USER_SEM_1); // Acceso Unico al flag de liberaciOn de hueco
			chof[chof_id].necesita_liberar = 0; // Al aparcar, prohibido liberar ningUn hueco
			signal_sem(USER_SEM_1);

			PARKING_aparcar(msg.hCoche, NULL, aparcar_commit, permiso_avance, permiso_avance_commit);

		} else if (msg.subtipo == PARKING_MSGSUB_DESAPARCAR){
			wait_sem(USER_SEM_1); // Acceso Unico al flag de liberaciOn de hueco
			chof[chof_id].necesita_liberar = 1; // Al desaparcar, estamos pendientes de dejar libre el hueco cuando nos vayamos (permiso_avance_commit)
			signal_sem(USER_SEM_1);

			PARKING_desaparcar(msg.hCoche, NULL, permiso_avance, permiso_avance_commit);
		}
		// Al terminar la maniobra, "borramos" nuestro coche temporalmente sacándolo del mapa
		wait_sem(USER_SEM_1);
		chof[chof_id].x = -1;
		chof[chof_id].y = -1; // Lo mandamos a Cuenca para que nadie choque con él mientras lee el buzón
		chof[chof_id].x_futuro = -1;
		chof[chof_id].y_futuro = -1;
		// Despierto a cualquiera que estuviera esperando
		for (int i = 0; i < nchof; i++) {
			if (chof[i].esperando_a == chof_id) {
				chof[i].esperando_a = -1;
				signal_sem(PARKING_getNSemAforos() + i); 
			}
		}
		signal_sem(USER_SEM_1);
	}
}

/* Ajustes */
int llegada_primer_ajuste(HCoche hc){ // FunciOn de llegada de coche a la primera acera
	int tamano_coche = PARKING_getLongitud(hc); // Averigua cuAnto mide el coche
    int huecos_consecutivos = 0; // Variable auxiliar para contar los huecos seguidos que encontramos en la zona de aparcamiento

    for(int i = 0; i<80; i++){
        if(memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][i] == 0){ // Si el hueco estA vacIo, incremento el contador
            
            huecos_consecutivos++;
            // Si el hueco es suficientemente grande como para que quepa el coche, devuelvo esa posiciOn para que el chOfer sepa dOnde aparcar
            if(tamano_coche == huecos_consecutivos){
                int posiciOn_aparcamiento = i - huecos_consecutivos + 1; // Devolvemos la primera posiciOn del hueco libre del aparcamiento

                // Bucle para reservar esa posiciOn antes de que nos la quite otro coche
                for(int j = posiciOn_aparcamiento; j<=i; j++){ 
                    memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][j] = 1; // Ponemos a 1 (ocupado) todos esos huecos que antes estaban a 0 (libre)
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

	/*
	int tam_mejor = 100;	// TamaNo del hueco mas justo (pequeNo)
	int pos_mejor = -1;
	int huecos_consecutivos = 0;

	wait_sem(USER_SEM_2);

	// Sitio mas justo
	for (int i=0; i<81; i++){
		if (i < 80 && memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][i] == 0){
			huecos_consecutivos++;
		} else {
			// LLegamos al final del hueco
			if (huecos_consecutivos >= PARKING_getLongitud(hc) && huecos_consecutivos <= tam_mejor){
				// Si es el heco mas justo hasta ahora guardamos su posicion
				tam_mejor = huecos_consecutivos;
				pos_mejor = i - huecos_consecutivos;
			}
			huecos_consecutivos=0;
		}
	}

    if (pos_mejor == -1){
		// No se encontrO
		signal_sem(USER_SEM_2);
		return -1;
	}
	// Reserva del hueco
	for(int i = pos_mejor; i<PARKING_getLongitud(hc)+pos_mejor; i++){ 
        memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][i] = 1; // Ponemos a 1 (ocupado) todos esos huecos que antes estaban a 0 (libre)
    }
	signal_sem(USER_SEM_2);

	return pos_mejor;

	*/


	return -2; // Por ahora ya que a velocidad 0 no funciona
}

int llegada_peor_ajuste(HCoche hc){ // FunciOn de llegada de coche a la cuarta acera
	// Sitio con mas espacio



    return -2; // Devolvemos -2 para que no moleste de momento en la ejecuciOn

}


/* Funciones de permisos */
void aparcar_commit(HCoche hc){	// Se ejecuta cuando el coche ha terminado de aparcar físicamente




}
// Se ejecuta para pedir permiso antes de realizar un movimiento
void permiso_avance(HCoche hc){
    int chocado = 1; // Empezamos asumiendo que al menos hay que mirar una vez en el while
    while(chocado){

        wait_sem(USER_SEM_1); // Protegemos la lectura de chof
        chocado = 0; // Asumimos que de momento no hubo choque
        chof[chof_id].esperando_a = -1; // Reiniciamos a quiEn estamos esperando por si es otra vuelta del bucle

        // Comprobar si el avance es vertical o horizontal
        if(PARKING_getX(hc) != PARKING_getX2(hc)){
            /* Avance horizontal */
            for (int i=0; i<nchof; i++){
                if (i == chof_id || chof[i].y == -1) continue; 
                
                // ¿Choco con dónde ESTÁ el coche ahora mismo? (Protege culos)
                int choca_actual = (PARKING_getY2(hc) == chof[i].y && 
                                    PARKING_getX2(hc) < (chof[i].x + chof[i].longitud) && 
                                    (PARKING_getX2(hc) + PARKING_getLongitud(hc)) > chof[i].x);
                
                // ¿Choco con hacia dónde VA el coche? (Evita colisiones frontales/laterales)
                int choca_futuro = (chof[i].y_futuro != -1 && 
                                    PARKING_getY2(hc) == chof[i].y_futuro && 
                                    PARKING_getX2(hc) < (chof[i].x_futuro + chof[i].longitud) && 
                                    (PARKING_getX2(hc) + PARKING_getLongitud(hc)) > chof[i].x_futuro);

                if(choca_actual || choca_futuro){
                    chof[chof_id].esperando_a = i;
                    chocado = 1;                     
                    break;
                }
            }
        }else{
            /* Avance vertical (Hacemos exactamente lo mismo pero sin la Y, como querías) */
            for(int i=0; i<nchof; i++){
                if(i == chof_id || chof[i].y == -1) continue;

                int choca_actual_v = (PARKING_getX(hc) < (chof[i].x + chof[i].longitud) && 
                                      (PARKING_getX(hc) + PARKING_getLongitud(hc)) > chof[i].x);
                                      
                int choca_futuro_v = (chof[i].x_futuro != -1 && 
                                      PARKING_getX(hc) < (chof[i].x_futuro + chof[i].longitud) && 
                                      (PARKING_getX(hc) + PARKING_getLongitud(hc)) > chof[i].x_futuro);

                if(choca_actual_v || choca_futuro_v){
                    chof[chof_id].esperando_a = i;
                    chocado = 1;
                    break;
                }
            }
        }

        if(chocado == 1){ // Si hemos detectado colisiOn, ya sea horizontal o vertical
            signal_sem(USER_SEM_1); // Dejo que el siguiente entre
            wait_sem(PARKING_getNSemAforos() + chof_id); // Wait sobre el semAforo de nuestro chOfer
            // Si aqui ya han despertado a este chOfer, vuelve a entrar en el while y comprueba por si hay otro coche bloqueando
        }else{
            // Reservo el siguiente paso que voy a dar, por si me quitan la posicion antes de dar el paso que el resto ya lo sepan
            chof[chof_id].x_futuro = PARKING_getX2(hc);
            chof[chof_id].y_futuro = PARKING_getY2(hc);

            signal_sem(USER_SEM_1); // Dejo que el siguiente entre
            // Como chocado serIa = 0, sale del while y el coche puede seguir avanzando hasta que encuentre otra colisiOn
        }
    }
}
// Se ejecuta justo despuEs de que el coche se ha movido
void permiso_avance_commit(HCoche hc){
    wait_sem(USER_SEM_1); // Protegemos el acceso a la memoria compartida

    // Actualizamos la posiciOn de este coche
    chof[chof_id].x = PARKING_getX(hc);
    chof[chof_id].y = PARKING_getY(hc);
    // Como ya hemos llegado, limpiamos la posiciOn futura
    chof[chof_id].x_futuro = -1;
    chof[chof_id].y_futuro = -1;

	// TODO: liberar las posiciones del parking cuando el coche ya haya salido a la carretera (y == 2)
    if(chof[chof_id].y == 2 && chof[chof_id].necesita_liberar == 1){ // El coche ha llegado a la carretera y estA desaparcando 
        int inicio = PARKING_getPosiciOnEnAcera(hc);
        int tam = PARKING_getLongitud(hc);
        for(int j = inicio; j < inicio + tam; j++){
            memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][j] = 0; // Liberamos el hueco que ocupaba el coche
        }
        
        chof[chof_id].necesita_liberar = 0; // Ya ha liberado una vez, asi que no lo vuelve a hacer
    }
    // Despertamos a los que estaban esperando a nuestro movimiento
    for(int i = 0; i < nchof; i++){
        if(chof[i].esperando_a == chof_id){ // Si el campo esperando de cualquiera de los chOferes estA esperando por mI, le mando signal, para que sepa que ya me movI
            chof[i].esperando_a = -1; // Limpio el campo de ese chOfer que esperaba por mI
            signal_sem(PARKING_getNSemAforos() + i); // Despertamos al coche que esperaba por nosotros
        }
    }
    signal_sem(USER_SEM_1); // Dejamos que entre el siguiente
}

// FunciOn de callback para no mezclar coches que aparcan con coches que desaparcan
/*void permiso_avance_commit_desaparcar(HCoche hc){
    if(PARKING_getY(hc) == 2){ //si el coche que estA desaparcando estA en la acera
        int inicio = PARKING_getPosiciOnEnAcera(hc);
        int tam = PARKING_getLongitud(hc);

        if (memoria_compartida->aceras[0][inicio] == 1) { // Comprobamos que esa zona sigue a 1
            // Ponemos a 0 (libre) los huecos exactos que ocupaba el coche
            for (int j = inicio; j < inicio + tam; j++) {
                memoria_compartida->aceras[0][j] = 0; 
            }

            // TODO: AquI pondremos un semAforo con SIGNAL para avisar a los coches que hay sitio nuevo
        }
    }
}*/


/* Funciones de semAforos */
// FunciOn para hacer un WAIT
void wait_sem(int num_semAforo) {
    struct sembuf sops[1]; // Actuamos solo sobre 1 semAforo
    sops[0].sem_num = num_semAforo; 
    sops[0].sem_op  = -1;           
    sops[0].sem_flg = 0;           
    if (semop(sem_id, sops, 1) == -1) {
        perror("Error al realizar wait_sem");
    }
}

// FunciON para hacer un SIGNAL
void signal_sem(int num_semAforo) {
    struct sembuf sops[1]; // Actuamos solo sobre 1 semAforo
    sops[0].sem_num = num_semAforo; 
    sops[0].sem_op  = 1;            
    sops[0].sem_flg = 0;
    if (semop(sem_id, sops, 1) == -1) {
        perror("Error al realizar signal_sem");
    }
}
