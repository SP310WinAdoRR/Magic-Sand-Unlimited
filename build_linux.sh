#!/bin/bash
set -e

# ==============================================================================
# Script de construcción para Magic Sand en Linux (Generador de AppImage)
# ==============================================================================
# Características:
# 1. Compilación reproducible con Docker sobre Ubuntu 16.04 (soporte glibc 2.23+).
# 2. Parche automático para Kinect v1 (K4W / Modelo 1473: FREENECT_DEVICE_CAMERA).
# 3. Shim de compatibilidad Wayland / XWayland para evitar crash de GLFW 3.1.
# 4. Gestión de datos mutables del usuario (~/Magic-Sand/data o ./data portable):
#    Evita el sistema de solo lectura del AppImage (SquashFS), permitiendo
#    leer y guardar calibraciones, XMLs de configuración, colormaps y puntajes.
# 5. Configuración automática de permisos y memoria de buffer de Kinect
#    en la primera ejecución (o mediante ./Magic-Sand-x86_64.AppImage --setup-kinect).
# ==============================================================================

# Comprobaciones iniciales del entorno anfitrión
echo "==> Verificando dependencias en el sistema anfitrión..."
if ! command -v docker > /dev/null 2>&1; then
    echo "ERROR: Docker no está instalado en este equipo. Por favor instálalo para continuar."
    exit 1
fi

if ! docker info > /dev/null 2>&1; then
    echo "ERROR: El servicio de Docker no está en ejecución o el usuario no tiene permisos para acceder a él."
    echo "Prueba iniciar el servicio (ej. 'sudo systemctl start docker') o añadir tu usuario al grupo 'docker'."
    exit 1
fi

if [ ! -f "setup_kinect_linux.sh" ]; then
    echo "ERROR: No se encontró el archivo setup_kinect_linux.sh en el directorio actual."
    exit 1
fi

# Limpieza segura al salir
cleanup() {
    rm -f Dockerfile.build build_inside_docker.sh
}
trap cleanup EXIT INT TERM

echo "==> [1/4] Generando Dockerfile.build..."
cat << 'DOCKER_EOF' > Dockerfile.build
FROM ubuntu:16.04

# Evitar prompts interactivos durante instalación de paquetes
ENV DEBIAN_FRONTEND=noninteractive

# Instalar dependencias de compilación y empaquetado
RUN apt-get update && apt-get install -y \
    wget \
    curl \
    git \
    build-essential \
    pkg-config \
    sudo \
    fuse \
    file \
    imagemagick \
    software-properties-common \
    && rm -rf /var/lib/apt/lists/*

# Descargar e instalar openFrameworks 0.9.3
WORKDIR /opt
RUN wget -q https://openframeworks.cc/versions/v0.9.3/of_v0.9.3_linux64_release.tar.gz && \
    tar -xzf of_v0.9.3_linux64_release.tar.gz && \
    mv of_v0.9.3_linux64_release of && \
    rm of_v0.9.3_linux64_release.tar.gz

WORKDIR /opt/of/scripts/linux/ubuntu
RUN sudo ./install_dependencies.sh -y && \
    sudo ./install_codecs.sh -y || true

WORKDIR /opt/of/libs/openFrameworksCompiled/project
RUN make -j$(nproc)

# Parchear ofxKinect para seleccionar solo FREENECT_DEVICE_CAMERA
# Kinect for Windows (K4W / Modelo 1473) falla si se solicitan motor o audio
RUN sed -i 's/FREENECT_DEVICE_MOTOR | FREENECT_DEVICE_CAMERA/FREENECT_DEVICE_CAMERA/g' /opt/of/addons/ofxKinect/src/ofxKinect.cpp

# Descargar addons requeridos por Magic Sand
WORKDIR /opt/of/addons
RUN git clone -b stable https://github.com/kylemcdonald/ofxCv.git && \
    git clone https://github.com/braitsch/ofxParagraph.git && \
    git clone https://github.com/thomwolf/ofxDatGui.git && \
    git clone https://github.com/braitsch/ofxModal.git

# Descargar linuxdeploy y appimagetool
WORKDIR /opt/appimage
RUN wget -q https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage && \
    chmod +x linuxdeploy-x86_64.AppImage
RUN wget -q https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage && \
    chmod +x appimagetool-x86_64.AppImage

WORKDIR /workspace

CMD ["/workspace/build_inside_docker.sh"]
DOCKER_EOF

echo "==> [2/4] Generando script de compilación interna (build_inside_docker.sh)..."
cat << 'DOCKER_EOF' > build_inside_docker.sh
#!/bin/bash
set -e

export OF_ROOT=/opt/of

# Limpiar objetos de compilaciones anteriores
rm -rf obj

# Asegurar parche de ofxKinect
sed -i 's/FREENECT_DEVICE_MOTOR | FREENECT_DEVICE_CAMERA/FREENECT_DEVICE_CAMERA/g' /opt/of/addons/ofxKinect/src/ofxKinect.cpp

echo "Compilando Magic-Sand..."
make APPNAME=Magic-Sand -j$(nproc)

# Asegurar nombre correcto de binario
if [ -f bin/workspace ] && [ ! -f bin/Magic-Sand ]; then
    mv bin/workspace bin/Magic-Sand
fi

echo "Estructurando AppDir..."
rm -rf AppDir
mkdir -p AppDir/usr/bin
mkdir -p AppDir/usr/share/applications
mkdir -p AppDir/usr/share/icons/hicolor/256x256/apps
mkdir -p AppDir/usr/share/magic-sand/data

# 1. Copiar ejecutable
cp bin/Magic-Sand AppDir/usr/bin/

# 2. Copiar plantilla maestra de datos a usr/share/magic-sand/data y usr/bin/data
mkdir -p AppDir/usr/share/magic-sand/data
cp -r bin/data/. AppDir/usr/share/magic-sand/data/
mkdir -p AppDir/usr/bin/data
cp -r bin/data/. AppDir/usr/bin/data/

# 3. Incluir script de configuración de permisos del sistema para Kinect
cp /workspace/setup_kinect_linux.sh AppDir/usr/bin/setup_kinect_linux.sh
chmod +x AppDir/usr/bin/setup_kinect_linux.sh

# 4. Generar archivo desktop
cat << 'DESKTOP_EOF' > AppDir/usr/share/applications/magicsand.desktop
[Desktop Entry]
Name=Magic Sand
Exec=Magic-Sand
Icon=magicsand
Type=Application
Categories=Game;Education;
Terminal=false
DESKTOP_EOF

# 5. Extraer icono en alta resolución
convert /workspace/icon.ico[5] AppDir/usr/share/icons/hicolor/256x256/apps/magicsand.png 2>/dev/null || \
convert /workspace/icon.ico[0] AppDir/usr/share/icons/hicolor/256x256/apps/magicsand.png

# 6. Desplegar dependencias compartidas con linuxdeploy
echo "Empaquetando bibliotecas compartidas con linuxdeploy..."
export APPIMAGE_EXTRACT_AND_RUN=1
/opt/appimage/linuxdeploy-x86_64.AppImage \
    --appdir AppDir \
    -d AppDir/usr/share/applications/magicsand.desktop \
    -i AppDir/usr/share/icons/hicolor/256x256/apps/magicsand.png \
    -e AppDir/usr/bin/Magic-Sand

# 7. Compilar shim de compatibilidad para Wayland / XWayland
echo "Compilando shim de compatibilidad XWayland..."
cat << 'EOF' > /tmp/xwayland_fix.c
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

static Bool (*real_XkbQueryExtension)(Display *, int *, int *, int *, int *, int *) = NULL;

Bool XkbQueryExtension(Display *dpy, int *opcode_rtrn, int *event_rtrn, int *error_rtrn, int *major_in_out, int *minor_in_out) {
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    const char *session_type = getenv("XDG_SESSION_TYPE");
    if (wayland_display || (session_type && strcmp(session_type, "wayland") == 0)) {
        return False;
    }
    if (!real_XkbQueryExtension) {
        real_XkbQueryExtension = dlsym(RTLD_NEXT, "XkbQueryExtension");
    }
    if (real_XkbQueryExtension) {
        return real_XkbQueryExtension(dpy, opcode_rtrn, event_rtrn, error_rtrn, major_in_out, minor_in_out);
    }
    return False;
}
EOF
gcc -shared -fPIC -O2 -o AppDir/usr/lib/libxwayland_fix.so /tmp/xwayland_fix.c -ldl
rm -f /tmp/xwayland_fix.c

# 8. Generar AppRun personalizado con gestión de datos mutables y setup de Kinect
echo "Generando script AppRun..."
rm -f AppDir/AppRun
cat << 'APPRUN_EOF' > AppDir/AppRun
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
# Si HERE resuelve dentro de un subdirectorio, apuntar a la raíz del AppDir
if [ ! -d "$HERE/usr/bin" ]; then
    if [ -d "$HERE/../usr/bin" ]; then
        HERE="$(readlink -f "$HERE/..")"
    elif [ -d "$HERE/../../usr/bin" ]; then
        HERE="$(readlink -f "$HERE/../..")"
    fi
fi
export APPDIR="$HERE"
export PATH="$HERE/usr/bin:$PATH"
export LD_LIBRARY_PATH="$HERE/usr/lib:$LD_LIBRARY_PATH"

# Aplicar shim para sesiones Wayland / XWayland
if [ -f "$HERE/usr/lib/libxwayland_fix.so" ]; then
    export LD_PRELOAD="$HERE/usr/lib/libxwayland_fix.so${LD_PRELOAD:+:$LD_PRELOAD}"
fi

# ==============================================================================
# A. Selección del directorio de datos mutables
# ==============================================================================
# Determinar la ubicación del archivo ejecutable .AppImage en el sistema anfitrión
if [ -n "$APPIMAGE" ]; then
    APPIMAGE_DIR="$(dirname "$(readlink -f "$APPIMAGE")")"
elif [ -n "$OWD" ]; then
    APPIMAGE_DIR="$OWD"
else
    APPIMAGE_DIR="$(dirname "$(readlink -f "$0")")"
fi

# Selección del directorio de datos:
# Prioridad 1: Variable de entorno manual (MAGIC_SAND_DATA u OF_DATA_PATH)
# Prioridad 2: Modo Portable (si existe previamente 'data' o 'Magic-Sand-Data' junto al AppImage o en el directorio actual)
# Prioridad 3: Modo Estándar en Home (~/Magic-Sand/data)

if [ -n "$MAGIC_SAND_DATA" ]; then
    DATA_ROOT="$MAGIC_SAND_DATA"
elif [ -n "$OF_DATA_PATH" ]; then
    DATA_ROOT="$OF_DATA_PATH"
elif [ -d "$APPIMAGE_DIR/data" ]; then
    DATA_ROOT="$APPIMAGE_DIR/data"
elif [ -d "$APPIMAGE_DIR/Magic-Sand-Data" ]; then
    DATA_ROOT="$APPIMAGE_DIR/Magic-Sand-Data"
elif [ -n "$OWD" ] && [ -d "$OWD/data" ]; then
    DATA_ROOT="$OWD/data"
elif [ -n "$OWD" ] && [ -d "$OWD/Magic-Sand-Data" ]; then
    DATA_ROOT="$OWD/Magic-Sand-Data"
else
    DATA_ROOT="${HOME:-/home/$USER}/Magic-Sand/data"
fi

DATA_CONTAINER="$(dirname "$DATA_ROOT")"
mkdir -p "$DATA_CONTAINER" 2>/dev/null || true

# ==============================================================================
# B. Soporte y configuración de Kinect v1 en el sistema anfitrión
# ==============================================================================
setup_kinect_system() {
    local SCRIPT="$HERE/usr/bin/setup_kinect_linux.sh"
    if [ ! -f "$SCRIPT" ]; then
        echo "! Script de configuración no encontrado: $SCRIPT"
        return 1
    fi

    echo "=========================================================="
    echo " Configurando permisos udev y memoria USB para Kinect v1"
    echo "=========================================================="

    if [ "$EUID" -eq 0 ]; then
        "$SCRIPT"
        return $?
    elif command -v pkexec > /dev/null 2>&1; then
        pkexec "$SCRIPT"
        return $?
    elif [ -t 0 ] && command -v sudo > /dev/null 2>&1; then
        sudo "$SCRIPT"
        return $?
    elif command -v kdialog > /dev/null 2>&1; then
        if kdialog --title "Magic Sand" --yesno "Se requiere configurar los permisos udev y memoria USB para el sensor Kinect en este sistema.\n\n¿Deseas aplicar la configuración ahora? (Requiere contraseña de administrador)"; then
            if command -v pkexec > /dev/null 2>&1; then
                pkexec "$SCRIPT"
            elif command -v sudo > /dev/null 2>&1; then
                sudo "$SCRIPT"
            fi
        fi
        return $?
    elif command -v zenity > /dev/null 2>&1; then
        if zenity --question --title="Magic Sand" --text="Se requiere configurar los permisos udev y memoria USB para el sensor Kinect en este sistema.\n\n¿Deseas aplicar la configuración ahora? (Requiere contraseña de administrador)"; then
            if command -v pkexec > /dev/null 2>&1; then
                pkexec "$SCRIPT"
            elif command -v sudo > /dev/null 2>&1; then
                sudo "$SCRIPT"
            fi
        fi
        return $?
    else
        echo "! No se detectó terminal ni interfaz gráfica para solicitar elevación de permisos."
        echo "  Por favor ejecuta manualmente: sudo $SCRIPT"
        return 1
    fi
}

# Ejecutar configuración si se solicita por parámetro
if [ "$1" = "--setup-kinect" ]; then
    setup_kinect_system
    exit $?
fi

# Comprobar si las reglas udev ya existen en el sistema anfitrión
KINECT_RULES="/etc/udev/rules.d/51-kinect.rules"
KINECT_PROMPT_FLAG="$DATA_CONTAINER/.kinect_prompted"

if [ ! -f "$KINECT_RULES" ]; then
    # Si no existen y no se ha preguntado previamente (o estamos en terminal)
    if [ ! -f "$KINECT_PROMPT_FLAG" ] || [ -t 0 ]; then
        echo "Reglas de Kinect no detectadas en el sistema. Iniciando configuración..."
        setup_kinect_system || true
        touch "$KINECT_PROMPT_FLAG" 2>/dev/null || true
    fi
fi

# Ajustar memoria de buffer USB a 64 MB en tiempo de ejecución solo si tenemos permisos de escritura
if [ -w /sys/module/usbcore/parameters/usbfs_memory_mb ]; then
    CURRENT_MB=$(cat /sys/module/usbcore/parameters/usbfs_memory_mb 2>/dev/null || echo "0")
    if [ "$CURRENT_MB" -lt 64 ] 2>/dev/null; then
        echo 64 > /sys/module/usbcore/parameters/usbfs_memory_mb 2>/dev/null || true
    fi
fi

# ==============================================================================
# C. Preparación y sincronización de datos mutables
# ==============================================================================
# Asegurar directorios requeridos por el código
mkdir -p "$DATA_ROOT/settings"
mkdir -p "$DATA_ROOT/colorMaps"
mkdir -p "$DATA_ROOT/mapGame/Scores"
mkdir -p "$DATA_ROOT/mapGame/ReferenceData"
mkdir -p "$DATA_ROOT/mapGame/DebugFiles"
mkdir -p "$DATA_ROOT/boidGame/DebugFiles"
mkdir -p "$DATA_ROOT/DebugFiles"

# Ubicación de los datos maestros incluidos en el AppImage
TEMPLATE_DATA="$HERE/usr/share/magic-sand/data"
[ ! -d "$TEMPLATE_DATA" ] && TEMPLATE_DATA="$HERE/usr/bin/data"

# Sincronizar archivos maestros si faltan, preservando modificaciones del usuario (-n: no-clobber)
if [ -d "$TEMPLATE_DATA" ]; then
    cp -rn "$TEMPLATE_DATA/." "$DATA_ROOT/" 2>/dev/null || true
fi

# Garantizar individualmente que todos los archivos XML vitales existan y no estén vacíos
for XML_REL in \
    "settings/calibration.xml" \
    "settings/kinectProjectorSettings.xml" \
    "settings/sandSurfaceRendererSettings.xml" \
    "mapGame/ReferenceData/MapReferenceSettings.xml" \
    "colorMaps/HeightColorMap.xml"
do
    TARGET_XML="$DATA_ROOT/$XML_REL"
    SOURCE_XML="$TEMPLATE_DATA/$XML_REL"
    if [ ! -s "$TARGET_XML" ] && [ -f "$SOURCE_XML" ]; then
        mkdir -p "$(dirname "$TARGET_XML")"
        cp -f "$SOURCE_XML" "$TARGET_XML"
    fi
done

# Garantizar permisos de lectura y escritura para el usuario
chmod -R u+rwX "$DATA_ROOT" 2>/dev/null || true

# Exportar variable de entorno de datos para openFrameworks
export OF_DATA_PATH="$DATA_ROOT"

echo "[Magic Sand] Carpeta de datos activa: $DATA_ROOT"

# Cambiar el directorio de trabajo al contenedor de data/ (para que rutas relativas 'data/...' sean válidas)
cd "$DATA_CONTAINER"

# Ejecutar el binario de Magic Sand pasando todos los argumentos
exec "$HERE/usr/bin/Magic-Sand" "$@"
APPRUN_EOF
chmod +x AppDir/AppRun

# 9. Crear el AppImage final con appimagetool
echo "Empaquetando con appimagetool..."
export APPIMAGE_EXTRACT_AND_RUN=1
/opt/appimage/appimagetool-x86_64.AppImage AppDir Magic-Sand-x86_64.AppImage
[ ! "Magic-Sand-x86_64.AppImage" -ef "/workspace/Magic-Sand-x86_64.AppImage" ] && mv Magic-Sand-x86_64.AppImage /workspace/ || true
chmod 777 /workspace/Magic-Sand-x86_64.AppImage

echo "==> AppImage creado exitosamente."
DOCKER_EOF
chmod +x build_inside_docker.sh

if [ "$1" = "--rebuild" ] || ! docker image inspect magic-sand-appimage-builder > /dev/null 2>&1; then
    echo "==> [3/4] Construyendo imagen de Docker..."
    docker build -t magic-sand-appimage-builder -f Dockerfile.build .
else
    echo "==> [3/4] Imagen 'magic-sand-appimage-builder' encontrada localmente. Reutilizando (usa --rebuild para forzar)."
fi

echo "==> [4/4] Ejecutando compilación y empaquetado dentro del contenedor Docker..."
docker run --rm -v "$(pwd):/workspace" --privileged magic-sand-appimage-builder

echo "=============================================================================="
echo " ¡Compilación completada exitosamente!"
echo " El archivo generado es: Magic-Sand-x86_64.AppImage"
echo ""
echo " Notas de uso:"
echo " - Datos mutables: Se guardan y cargan en ~/Magic-Sand/data/"
echo "   (o en una carpeta ./data junto al AppImage si se usa en modo portable)."
echo " - Primera ejecución: Solicitará permisos de superusuario para instalar"
echo "   las reglas udev de Kinect y memoria de buffer USB."
echo " - También puedes configurar Kinect manualmente ejecutando:"
echo "   ./Magic-Sand-x86_64.AppImage --setup-kinect"
echo "=============================================================================="
