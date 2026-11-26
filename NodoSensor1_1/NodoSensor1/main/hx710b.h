#ifndef HX710B_H
#define HX710B_H

#include <stdbool.h>
#include <stdint.h>

/** Inicializa el convertidor HX710B conectado a los pines configurados. */
bool hx710b_init(void);

/**
 * Lee la entrada diferencial del HX710B.
 *
 * El resultado son cuentas ADC con signo de 24 bits. Su conversion a una
 * unidad fisica (presion, peso o nivel) depende de la calibracion del sensor.
 */
bool hx710b_read_raw(int32_t *value);

#endif
