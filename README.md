# IRIS

IRIS es un sistema operativo propio orientado a desarrollo y gaming.

## Visión
IRIS busca convertirse en una plataforma unificada para:
- desarrollo de software
- ejecución de videojuegos
- control profundo del hardware
- personalización total del entorno

## Objetivos iniciales
- Boot por UEFI
- Arquitectura inicial x86_64
- Kernel híbrido modular
- Sistema de autenticación unificado por usuario
- Base preparada para subsistema gráfico y drivers por vendor

## Estructura inicial
boot/
kernel/
userland/
drivers/
tools/
scripts/
build/
docs/
tests/

## Estado actual
Fase 0:
- creación del repositorio
- instalación de dependencias
- estructura base del proyecto
- preparación de documentación inicial

## Próximos pasos
1. definir toolchain y flujo de compilación
2. preparar boot UEFI mínimo
3. crear kernel_main
4. habilitar salida por consola serial
5. arrancar en QEMU
