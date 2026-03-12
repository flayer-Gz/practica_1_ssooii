# Primera Práctica SSOOII

1. Hacer el código necesario para gestionar los argumentos que se le pasan al programa

2. Crear los semáforos, la memoria comparida y el buzón, y comprobad que se crean bien, con ipcs. Es preferible, para que no haya interferencias, que los defináis privados.

3. Registrar SIGINT para que cuando se pulse ^C se eliminen los recursos IPC. Lograr que si el programa acaba normalmente o se produce cualquier error, también se eliminen los recursos (mandad una señal SIGINT en esos casos al proceso padre).

4. Llamar a la función PARKING_inicio en main. Debe aparecer la pantalla de bienvenida y, pasados dos segundos, dibujarse la pantalla. Para el array de funciones de llegada, usad funciones que nada más tengan un pause() dentro, de momento. Todos los procesos que se creen a continuación en sucesivos puntos deben crearse después de haber llamado a PARKING_inicio.

5. Dejad a padre en pause() y cread el hijo que se encarga de contar el tiempo y avisa al padre llamando a PARKING_fin(). Recordad que tenéis que usar SIGALRM para eso y no sleep

6. A continuación, haced que el padre llame a PARKING_simulaciOn()

7. Los siguientes pasos conllevan resolver la práctica solo para un algoritmo y con un chófer, para ir, poco a poco, aumentando la dificultad. Para ello, solo programad la función de llegada del primer algoritmo. El resto de funciones de llegada, que devuelvan -2 para indicar a la biblioteca que no queréis, de momento, tratar con ellas. Y el chófer, que solo lea el mensaje y lo imprima en la pantalla.

8. Que el chófer llame ahora a PARKING_aparcar. Podéis hacer que las funciones de commits y permisos, de momento, solo pongan un mensaje de depuración.

9. Que el chófer distinga entre los dos tipos de mensajes y llame a PARKING_aparcar o PARKING_desaparcar, según corresponda. Ya deberíais ser capaces de ver a los coches aparcar y salir, cuando toque.

10. Las cosas se van a complicar ahora, cuando añadáis varios chóferes: puede haber errores de orden de aparcamiento y de choques. Debéis introducir mecanismos de sincronización para que todo funcione.

11. Acabad la práctica y probadla a velocidad normal y a velocidad cero.

12. Meted la opción de diferentes políticas de gestión de la cola de tareas de los chóferes: FIFO, prioridad de aparcamientos y prioridad de desaparcamientos.

13. Añadid el resto de algoritmos

14. Diseñad la forma de acabar sin problemas y llamad a la función PARKING_fin().

15. Pulid los últimos detalles.
