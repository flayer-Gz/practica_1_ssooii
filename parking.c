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
#include <errno.h>

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
	int mi_acera;   // Acera a la que corresponde el turno de este chofer
} InfoChofer;

/* Recursos compartidos*/
typedef struct {
	int aceras[4][80];		// Las 4 aceras cada una con 80 espacios (0=libre, 1=ocupado)
	int turno_aparcar[4];		// Siguiente turno que toca aparcar, uno por acera
	int turno_dispensador[4];	// Siguiente turno que da la biblioteca, uno por acera
    int ultimo_sig_ajuste;  // Marcador del Ultimo que aparcO en esa acera
} DatosCompartidos;


/* Macros */
#define NUM_USER_SHM (sizeof(DatosCompartidos) + 8)		// Bytes de memoria comp. para el usuario (con un poco de mArgen por si acaso)
#define NUM_USER_SEM 2									// SemAforos para el usuario
#define USER_SEM_1 PARKING_getNSemAforos()+nchof		// Despues de los semaforos de la biblioteca y los choferes
#define USER_SEM_2 PARKING_getNSemAforos()+nchof+1


/* Prototipos */
int limpiar_recursos();

void manejador_SIGINT(int sig);
void manejador_SIGALRM(int sig);

int llegada_primer_ajuste(HCoche hc);
int llegada_siguiente_ajuste(HCoche hc);
int llegada_mejor_ajuste(HCoche hc);
int llegada_peor_ajuste(HCoche hc);

void chofer(int n, int PA, int PD);
void gestor_prioridades(int PA, int PD);

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
int chof_id = -1;



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

    // El array de la posiciOn de los chOferes empieza justo donde acaba la estructura
    chof = (InfoChofer *)(memoria_compartida + 1);

    /* Inicializamos las 4 aceras a 0 (libre) */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 80; j++) {
    	    memoria_compartida->aceras[i][j] = 0;
        }
    }
	/* Inicializamos los turnos y el marcador de posicion de siguiente_ajuste*/
	for (int i = 0; i < 4; i++){
		memoria_compartida->turno_aparcar[i] = 0;
		memoria_compartida->turno_dispensador[i] = 0;
	}
    memoria_compartida->ultimo_sig_ajuste = 0;

    /* Mandamos todos los choferes a -1 antes de empezar por si durante la ejecuciOn con 4 chOferes por ejemplo sOlo estAn 2 antendiendo que no bloqueen al quedarse en la (x,y)=0,0*/
    for(int i = 0; i < nchof; i++){
        chof[i].x = -1;
        chof[i].y = -1; 
        chof[i].x_futuro = -1;
        chof[i].y_futuro = -1;
        chof[i].esperando_a = -1;
        chof[i].necesita_liberar = 0;
        chof[i].longitud = 0;
		chof[i].mi_turno = -1;	// Valor invalido para que no pueda coincidir por error
		chof[i].mi_acera = -1;
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


    /* EjecuciOn */
    if (PARKING_inicio(velocidad, algoritmos_llegada, sem_id, buz_id, shm_id, D) == -1){
        perror("Error al ejecutar PARKING_inicio.");
        kill(getpid(), SIGINT);
		return 1;
    }

    
	/* CreaciOn procesos hijos (chOferes, cronOmetro y gestor de cola si es necesario) */
    int num_hijos = nchof + 1; // ChOferes y cronOmetro
    if(PA == 1 || PD == 1){
        num_hijos++; // Necesitaremos un hueco mAs para el gestor
    }
	pid = malloc((num_hijos)*sizeof(pid_t));

	for (int i=0; i<num_hijos; i++){
	    switch(pid[i]=fork()){
	        case -1:
	            perror("Error en fork");
	            kill(getpid(), SIGINT);	// Envia la señal SIGINT y salta a la manejadora
	            break;
	        case 0:
				// Hijos
				if(i == 0){ // Es el hijo cronOmetro
                    alarm(30);	// Programa la señal SIGALRM para dentro de 30s
					pause();	// Espera a SIGALRM
				}else if(i >= 1 && i <= nchof){ // Es cualquiera de los hijos chOfer
					// ChOferes
					chofer(i-1, PA, PD);	// -1 para que estEn numerados desde el 0
				}else{ // Es el hijo opcional del gestor
                    gestor_prioridades(PA, PD);
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
void manejador_SIGINT(int sig){	// Importante matar los procesos antes de borrar los IPCs para 
	if (getpid() == pid_padre){	// que no intenten acceder a ellos cuando han sido borrados
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
		// Limpieza de recursos
    	write(STDOUT_FILENO, "\nLiberando recursos...\n", 23);
    	if (limpiar_recursos()){
			write(STDOUT_FILENO, "No se han podido liberar los recursos.\n", 39);
			return;
    	} else
			write(STDOUT_FILENO, "Recursos liberados correctamente.\n", 34);

		_exit(0);
	}
}
void manejador_SIGALRM(int sig){
	PARKING_fin(1);
    _exit(0);
}

/* FunciOn que ejecutan todos los chOferes */
void chofer(int n, int PA, int PD){
	struct PARKING_mensajeBiblioteca msg;
	chof_id = n;

    // Determinamos cual va a ser el tipo de cola de prioridad que tenga el chOfer
    long tipo_lectura = PARKING_MSG; // FIFO por defecto
    if(PA == 1 || PD == 1){ // Si hay alguna prioridad distinta a FIFO activa
        tipo_lectura = -2; // Usamos el truco de coger el tipo de mensaje mAs bajo
    }
	while(42){
		wait_sem(USER_SEM_2);

		if (msgrcv(buz_id, &msg, sizeof(msg) - sizeof(long), tipo_lectura, 0) == -1){	// sizeof(msg) - sizeof(long) porque el campo tipo no cuenta para el mensaje
			perror("[CHOFER] Error al leer del buzón");
			break;
		}
		
		if (msg.subtipo == PARKING_MSGSUB_APARCAR){
			wait_sem(USER_SEM_1); // Acceso Unico al flag de liberaciOn de hueco
			chof[chof_id].necesita_liberar = 0; // Al aparcar, prohibido liberar ningUn hueco

			/* Asignacion de turno por acera */
			int acera = PARKING_getAlgoritmo(msg.hCoche);
			chof[chof_id].mi_acera = acera;
			chof[chof_id].mi_turno = memoria_compartida->turno_dispensador[acera]++;
			signal_sem(USER_SEM_2);	// Se deja leer el buzon a otro una vez hemos cogido turno

			/* ComprobaciOn de turno */
  			while (memoria_compartida->turno_aparcar[acera] != chof[chof_id].mi_turno){
				  signal_sem(USER_SEM_1);	// Suelta el mutex para no quedarse dormido con el
				  wait_sem(PARKING_getNSemAforos() + chof_id);  // dormimos
				  wait_sem(USER_SEM_1);
			}
			// Una vez es nuestro turno reservamos la posicion
			chof[chof_id].longitud = PARKING_getLongitud(msg.hCoche);
			chof[chof_id].x = PARKING_getX(msg.hCoche);
			chof[chof_id].y = PARKING_getY(msg.hCoche);
			signal_sem(USER_SEM_1);
				
			PARKING_aparcar(msg.hCoche, NULL, aparcar_commit, permiso_avance, permiso_avance_commit);

		} else if (msg.subtipo == PARKING_MSGSUB_DESAPARCAR){
			signal_sem(USER_SEM_2); // Se deja que otro lea el buzOn
			wait_sem(USER_SEM_1); // Acceso Unico al flag de liberaciOn de hueco
			chof[chof_id].mi_acera = PARKING_getAlgoritmo(msg.hCoche);
			chof[chof_id].longitud = PARKING_getLongitud(msg.hCoche);
			chof[chof_id].x = PARKING_getX(msg.hCoche);
			chof[chof_id].y = PARKING_getY(msg.hCoche);
			chof[chof_id].necesita_liberar = 1; // Al desaparcar, estamos pendientes de dejar libre el hueco cuando nos vayamos (permiso_avance_commit)
			signal_sem(USER_SEM_1);

			PARKING_desaparcar(msg.hCoche, NULL, permiso_avance, permiso_avance_commit);
		} else {
			// Si llega un tipo de mensaje desconocido
            signal_sem(USER_SEM_2);
		}
		// Al terminar la maniobra, "borramos" nuestro coche temporalmente sacándolo del mapa
		wait_sem(USER_SEM_1);
		chof[chof_id].x = -1;
		chof[chof_id].y = -1; // Lo mandamos a Cuenca para que nadie choque con él mientras lee el buzón
		chof[chof_id].x_futuro = -1;
		chof[chof_id].y_futuro = -1;
        chof[chof_id].mi_turno = -1;
        chof[chof_id].mi_acera = -1;
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

void gestor_prioridades(int PA, int PD){ // FunciOn encargada de gestionar las prioridades de cola (SOlo se ejecuta si PA o PD estAn activos)
    struct PARKING_mensajeBiblioteca msg;

    while (42) {
        // Lee Unica y exclusivamente los mensajes crudos de la biblioteca
        if (msgrcv(buz_id, &msg, sizeof(msg) - sizeof(long), PARKING_MSG, 0) == -1) {
            break; // Si el padre destruye el buzOn al salir, el msgrcv falla y el hijo muere en paz
        }

        // TraducciOn de prioridades
        if (PA == 1) {
            if (msg.subtipo == PARKING_MSGSUB_APARCAR) msg.tipo = 1; // Urgente
            else msg.tipo = 2; // Secundario
        } else if (PD == 1) {
            if (msg.subtipo == PARKING_MSGSUB_DESAPARCAR) msg.tipo = 1; // Urgente
            else msg.tipo = 2; // Secundario
        }

        // Reenvía el mensaje al mismo buzón con su nueva etiqueta
        if (msgsnd(buz_id, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
            break;
        }
    }
}

/* Ajustes */
int llegada_primer_ajuste(HCoche hc){ // FunciOn de llegada de coche a la primera acera
	int tamano_coche = PARKING_getLongitud(hc); // Averigua cuAnto mide el coche
    int huecos_consecutivos = 0; // Variable auxiliar para contar los huecos seguidos que encontramos en la zona de aparcamiento

	wait_sem(USER_SEM_1);
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
				signal_sem(USER_SEM_1);
                return posiciOn_aparcamiento;
        	}
        }else{
            // Si el hueco no estA vacIo (tendrA un 1), reinicio el contador de huecos
            huecos_consecutivos = 0;
        }
    }
    // El bucle a acabado y no ha econtrado hueco para ese coche, no puede aparcar aUn
	signal_sem(USER_SEM_1);
    return -1; // Todos los sitios ocupados/no suficientemente grandes
    //return -2;
}

int llegada_siguiente_ajuste(HCoche hc){ // FunciOn de llegada de coche a la segunda acera
    wait_sem(USER_SEM_1);
    int pos_ultimo = memoria_compartida->ultimo_sig_ajuste; // Guardamos dOnde aparcO el Ultimo
    int tamano_coche = PARKING_getLongitud(hc); // Averigua cuAnto mide el coche
    int huecos_consecutivos = 0; 

    // Si el hueco que dejO el Ultimo ya se liberO, retrocedemos hasta el incio del parking o hasta que encontremos el coche que estA detrAs en el parking
    if (memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][pos_ultimo] == 0) {
        while (pos_ultimo > 0 && memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][pos_ultimo - 1] == 0) {
            pos_ultimo--; // Caminamos hacia atrás
        }
    }
    //Buscamos desde el ULtimo hasta el final del parking
    for(int i = pos_ultimo; i<80; i++){
        if(memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][i] == 0){ // Si el hueco estA vacIo, incremento el contador
            
            huecos_consecutivos++;
            // Si el hueco es suficientemente grande como para que quepa el coche y la celda coincide con la siguiente al ULtimo que aparcO devuelvo esa posiciOn para que el chOfer sepa dOnde aparcar
            if(tamano_coche == huecos_consecutivos){
                int posiciOn_aparcamiento = i - huecos_consecutivos + 1; // Devolvemos la primera posiciOn del hueco libre del aparcamiento

                // Bucle para reservar esa posiciOn antes de que nos la quite otro coche
                for(int j = posiciOn_aparcamiento; j<=i; j++){ 
                    memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][j] = 1; // Ponemos a 1 (ocupado) todos esos huecos que antes estaban a 0 (libre)
                }
                if(i==79){
                    memoria_compartida->ultimo_sig_ajuste = 0; // La acera termina en 79 y debe volver a empezar desde el 0

                }else{
                    memoria_compartida->ultimo_sig_ajuste = i + 1; // Guardamos la posiciOn siguiente al Ultimo que aparcO
                }
				signal_sem(USER_SEM_1);
                return posiciOn_aparcamiento;
        	}
        }else{
            // Si el hueco no estA vacIo (tendrA un 1), reinicio el contador de huecos
            huecos_consecutivos = 0;
        }
    }

    // Si entramos aqui quiere decir que hemos llegado al final de la calle y no hemos encontrado hueco, por lo tanto volvemos a buscar desde el inicio
    huecos_consecutivos = 0; // Reiniciamos el contador para con partir coches a la mitad
    for(int i = 0; i < pos_ultimo; i++){
        if(memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][i] == 0){ 
            
            huecos_consecutivos++;
            
            if(tamano_coche == huecos_consecutivos){
                int posiciOn_aparcamiento = i - huecos_consecutivos + 1; 

                // Reservamos
                for(int j = posiciOn_aparcamiento; j <= i; j++){ 
                    memoria_compartida->aceras[PARKING_getAlgoritmo(hc)][j] = 1; 
                }
                
                memoria_compartida->ultimo_sig_ajuste = i + 1; // Guarda la siguiente posiciOn dOnde tendrA que empezar a buscar sitio el siguiente
                
                signal_sem(USER_SEM_1);
                return posiciOn_aparcamiento;
            }
        }else{
            huecos_consecutivos = 0;
        }
    }
    // El bucle a acabado y no ha econtrado hueco para ese coche, no puede aparcar aUn
	signal_sem(USER_SEM_1);
    return -1; // Todos los sitios ocupados/no suficientemente grandes
    //return -2;
}

int llegada_mejor_ajuste(HCoche hc){ // FunciOn de llegada de coche a la tercera acera
    int tamano_coche = PARKING_getLongitud(hc);
    int algoritmo = PARKING_getAlgoritmo(hc);
    int tam_mejor = 100;    
    int pos_mejor = -1;
    int huecos_consecutivos = 0;

    wait_sem(USER_SEM_1);

    for (int i = 0; i <= 80; i++) {
        if (i < 80 && memoria_compartida->aceras[algoritmo][i] == 0) {
            huecos_consecutivos++;
        } else {
            // Evaluamos el hueco al encontrar un coche (1) o al llegar al borde final (80)
            if (huecos_consecutivos >= tamano_coche && huecos_consecutivos < tam_mejor) {
                tam_mejor = huecos_consecutivos;
                pos_mejor = i - huecos_consecutivos;
            }
            huecos_consecutivos = 0;
        }
    }

    if (pos_mejor == -1) { // No hemos encontrado hueco vAlido
        signal_sem(USER_SEM_1);
        return -1;
    }

    // Reserva del hueco solo del tamaNo del coche
    for(int j = pos_mejor; j < pos_mejor + tamano_coche; j++) { 
        memoria_compartida->aceras[algoritmo][j] = 1; 
    }
    
    signal_sem(USER_SEM_1);
    return pos_mejor;
	//return -2;
}

int llegada_peor_ajuste(HCoche hc){ // FunciOn de llegada de coche a la cuarta acera
    int tamano_coche = PARKING_getLongitud(hc);
    int algoritmo = PARKING_getAlgoritmo(hc);
    int tam_peor = 0;    
    int pos_peor = -1;
    int huecos_consecutivos = 0;

    wait_sem(USER_SEM_1);

    for (int i = 0; i <= 80; i++) {
        if (i < 80 && memoria_compartida->aceras[algoritmo][i] == 0) {
            huecos_consecutivos++;
        } else {
            // Evaluamos el hueco al encontrar un coche (1) o al llegar al borde final (80)
            if (huecos_consecutivos >= tamano_coche && huecos_consecutivos > tam_peor) {
                tam_peor = huecos_consecutivos;
                pos_peor = i - huecos_consecutivos;
            }
            huecos_consecutivos = 0;
        }
    }

    if (pos_peor == -1) { // No hemos encontrado hueco vAlido
        signal_sem(USER_SEM_1);
        return -1;
    }

    // Reserva del hueco solo del tamaNo del coche
    for(int j = pos_peor; j < pos_peor + tamano_coche; j++) { 
        memoria_compartida->aceras[algoritmo][j] = 1; 
    }
    
    signal_sem(USER_SEM_1);
    return pos_peor;


    //return -2; // Devolvemos -2 para que no moleste de momento en la ejecuciOn

}


/* Funciones de permisos */
void aparcar_commit(HCoche hc){	// Se ejecuta cuando el coche ha terminado de aparcar físicamente
	int acera = PARKING_getAlgoritmo(hc);
	wait_sem(USER_SEM_1);
	memoria_compartida->turno_aparcar[acera]++;
	// Despertar al chófer de esa acera que tiene ese nuevo turno
	for (int i = 0; i < nchof; i++) {
	    if (chof[i].mi_acera == acera &&
	        chof[i].mi_turno == memoria_compartida->turno_aparcar[acera]){
	        signal_sem(PARKING_getNSemAforos() + i);
	        break;
	    }
	}
	signal_sem(USER_SEM_1);
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
                if (chof[i].mi_acera != chof[chof_id].mi_acera) continue;	// Si no estA en mi acera no compruebo
                
                // ¿Choco con dOnde estA el coche ahora mismo?
                int choca_actual = (PARKING_getY2(hc) == chof[i].y && 
                                    PARKING_getX2(hc) < (chof[i].x + chof[i].longitud) && 
                                    (PARKING_getX2(hc) + PARKING_getLongitud(hc)) > chof[i].x);
                
                // ¿Choco con hacia dOnde va el coche? 
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
            /* Avance vertical */
            for(int i=0; i<nchof; i++){
                if(i == chof_id || chof[i].y == -1) continue;
                if(chof[i].mi_acera != chof[chof_id].mi_acera) continue;	// Si no estA en mi acera no compruebo

                int choca_actual_v = (PARKING_getY2(hc) == chof[i].y && PARKING_getX(hc) < (chof[i].x + chof[i].longitud) &&
									 (PARKING_getX(hc) + PARKING_getLongitud(hc)) > chof[i].x);
                                      
                int choca_futuro_v = (chof[i].y_futuro != -1 && PARKING_getY2(hc) == chof[i].y_futuro && 
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

	// Liberar las posiciones del parking cuando el coche ya haya salido a la carretera (y == 2)
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


/* Funciones de semAforos INMUNES A SEÑALES */
void wait_sem(int num_semAforo) {
    struct sembuf sops[1]; 
    sops[0].sem_num = num_semAforo; 
    sops[0].sem_op  = -1;
    sops[0].sem_flg = 0;            
    while (semop(sem_id, sops, 1) == -1) {
        if (errno == EINTR) continue; // Si nos interrumpe una señal, insistimos
        perror("Error crítico al realizar wait_sem");
        break;
    }
}

void signal_sem(int num_semAforo) {
    struct sembuf sops[1]; 
    sops[0].sem_num = num_semAforo; 
    sops[0].sem_op  = 1;            
    sops[0].sem_flg = 0;
    while (semop(sem_id, sops, 1) == -1) {
        if (errno == EINTR) continue; // Si nos interrumpe una señal, insistimos
        perror("Error crítico al realizar signal_sem");
        break;
    }
}
