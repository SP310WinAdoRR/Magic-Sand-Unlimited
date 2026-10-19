#!/bin/bash
set -e

# Script de configuración para Kinect v1 en sistemas Linux (Ubuntu, Linux Mint, Debian)
# Requiere permisos de superusuario (sudo)

if [ "$EUID" -ne 0 ]; then
  echo "Este script requiere privilegios de superusuario (root o sudo):"
  echo "  sudo ./setup_kinect_linux.sh"
  exit 1
fi

echo "=============================================="
echo " Configurando soporte para Kinect en Linux"
echo "=============================================="

# 1. Instalar reglas udev
echo "[1/3] Configurando reglas udev en /etc/udev/rules.d/51-kinect.rules..."
cat << 'EOF' > /etc/udev/rules.d/51-kinect.rules
# Kinect v1 (Xbox 360)
SUBSYSTEM=="usb", ATTR{idVendor}=="045e", ATTR{idProduct}=="02b0", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="045e", ATTR{idProduct}=="02ad", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="045e", ATTR{idProduct}=="02ae", MODE="0666", TAG+="uaccess"

# Kinect for Windows v1 (Model 1473 / K4W)
SUBSYSTEM=="usb", ATTR{idVendor}=="045e", ATTR{idProduct}=="02c2", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="045e", ATTR{idProduct}=="02be", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="045e", ATTR{idProduct}=="02bf", MODE="0666", TAG+="uaccess"
EOF

# Recargar reglas udev
if which udevadm > /dev/null 2>&1; then
  udevadm control --reload-rules || true
  udevadm trigger || true
  echo "✓ Reglas udev instaladas y recargadas."
else
  echo "! udevadm no encontrado, reinicia el sistema para aplicar las reglas."
fi

# 2. Aumentar memoria de buffer USB (usbfs_memory_mb)
echo "[2/3] Aumentando usbfs_memory_mb a 64 MB..."
if [ -f /sys/module/usbcore/parameters/usbfs_memory_mb ]; then
  echo 64 > /sys/module/usbcore/parameters/usbfs_memory_mb
  echo "✓ Memoria de buffer USB establecida temporalmente en 64 MB."
else
  echo "! Módulo usbcore no cargado como módulo externo o ruta no disponible en runtime."
fi

# 3. Configurar usbfs_memory_mb de forma permanente en modprobe y GRUB si aplica
echo "[3/3] Comprobando configuración permanente..."
# A) Configurar en modprobe si existe el directorio
if [ -d /etc/modprobe.d ]; then
  echo "options usbcore usbfs_memory_mb=64" > /etc/modprobe.d/usbcore.conf
  echo "✓ Archivo /etc/modprobe.d/usbcore.conf configurado."
fi

# B) Configurar en GRUB si aplica
if [ -f /etc/default/grub ]; then
  if grep -q "usbfs_memory_mb" /etc/default/grub; then
    echo "✓ usbfs_memory_mb ya está presente en /etc/default/grub."
  else
    echo "Agregando usbcore.usbfs_memory_mb=64 a /etc/default/grub..."
    sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="/GRUB_CMDLINE_LINUX_DEFAULT="usbcore.usbfs_memory_mb=64 /' /etc/default/grub
    if which update-grub > /dev/null 2>&1; then
      update-grub
      echo "✓ GRUB actualizado exitosamente con update-grub."
    elif which grub-mkconfig > /dev/null 2>&1 && [ -f /boot/grub/grub.cfg ]; then
      grub-mkconfig -o /boot/grub/grub.cfg
      echo "✓ GRUB actualizado exitosamente con grub-mkconfig."
    fi
  fi
fi

echo ""
echo "=============================================="
echo " Configuración completada."
echo " RECOMENDACIONES DE CONECTIVIDAD:"
echo " 1. Para garantizar estabilidad en la transferencia de datos isócronos de Kinect v1,"
echo "    conecte el sensor a un puerto USB 2.0 nativo o a un concentrador USB 2.0 dedicado."
echo " 2. Asegúrese de que el sensor cuente con su fuente de alimentación externa conectada (12V)."
echo "=============================================="

