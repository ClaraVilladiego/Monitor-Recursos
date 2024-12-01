# Sistema de Monitoreo de Recursos con Notificaciones Automáticas

Este proyecto implementa un sistema multihilo para monitorizar el uso de CPU, memoria y procesos en un entorno Linux. Además, genera notificaciones automáticas cuando los recursos exceden los umbrales configurados.


## Requisitos del sistema 

### Hardware Recomendado:
Procesador: Mínimo 2 núcleos para soportar la ejecución multihilo.
Memoria RAM: Al menos 1 GB para una ejecución fluida.
Espacio en disco: Espacio suficiente para almacenar logs y archivos temporales.


### Sistema operativo: 
Linux: Se recomienda un sistema Linux nativo, una máquina virtual con Linux, o el subsistema WSL en Windows.


### Entorno de desarrollo: 
IDE: Recomendado Visual Studio Code (opcional).


## Dependencias del Sistema

Asegúrate de que las siguientes bibliotecas y herramientas estén instaladas:

ncurses: Para la gestión de interfaces de texto en la terminal.
pthread: Manejo de hilos (incluido en las bibliotecas estándar de C en Linux).
glib: Proporciona funciones esenciales y soporte para notificaciones.
libnotify: Sistema de notificaciones para Linux.


### Instalación de dependencias:

Ejecuta el siguiente comando para instalar las dependencias necesarias:
sudo apt update
sudo apt install libncurses5-dev libncursesw5-dev libpthread-stubs0-dev libglib2.0-dev libnotify-dev build-essential


### Compilación del programa:

Para compilar el programa, utiliza el siguiente comando:

gcc -o monitor-recursos monitor-recursos.c -lncurses -lnotify -lpthread -lgobject-2.0 -lglib-2.0 -I/usr/include/glib-2.0 -I/usr/include/gdk-pixbuf-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include

### Ejecución del programa:

Una vez compilado, ejecuta el programa desde el terminal con:

./monitor-recursos
