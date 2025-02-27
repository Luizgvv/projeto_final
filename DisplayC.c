#include <stdio.h>
#include "hardware/pio.h"
#include "ws2812.pio.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "inc/ssd1306.h"
#include "inc/font.h"


#define I2C_PORT i2c1
#define I2C_SDA 14
#define I2C_SCL 15
#define endereco 0x3C

#define PINO_UMIDADE 26  // Pino ADC0 (A0)
#define PINO_RELE    6   // Relé (bomba d'agua)
#define PINO_LED_VERMELHO 13  // LED vermelho (umidade baixa)
#define PINO_LED_VERDE 11  // LED verde
#define PINO_LED_AZUL 12  // LED azul (irrigação manual)
#define PINO_BUZZER  21  // Alerta sonoro
#define JOYSTICK_Y_PIN 27  // Pino ADC1 (eixo Y do joystick)
#define BOTAO_A 5  // Botão A para irrigação manual
#define BOTAO_MODO 6  // Botão para alternar entre modos
#define LIMITE_UMIDADE_BAIXA 2000  // Limite para umidade baixa
#define LIMITE_UMIDADE_ALTA 3500   // Limite para umidade alta

// Matriz de LEDs 5x5 (usando GPIOs de 0 a 24)
#define LED_COUNT 25
#define LED_PIN 7
struct pixel_t {
    uint8_t G, R, B;
};
typedef struct pixel_t pixel_t;
typedef pixel_t npLED_t;

npLED_t leds[LED_COUNT];
PIO np_pio;
uint sm;

// Inicializa WS2812
void npInit(uint pin) {
    uint offset = pio_add_program(pio0, &ws2812_program);
    np_pio = pio0;
    sm = pio_claim_unused_sm(np_pio, true);
    ws2812_program_init(np_pio, sm, offset, pin, 800000.f, false);

    for (uint i = 0; i < LED_COUNT; ++i) {
        leds[i].R = 0;
        leds[i].G = 0;
        leds[i].B = 0;
    }
}

void npSetLED(const uint index, const uint8_t r, const uint8_t g, const uint8_t b) {
    leds[index].R = r;
    leds[index].G = g;
    leds[index].B = b;
}

void npClear() {
    for (uint i = 0; i < LED_COUNT; ++i) {
        npSetLED(i, 0, 0, 0);
    }
}

void npWrite() {
    for (uint i = 0; i < LED_COUNT; ++i) {
        pio_sm_put_blocking(np_pio, sm, (leds[i].G << 16) | (leds[i].R << 8) | leds[i].B);
    }
    sleep_us(100);
}

// Função para atualizar a matriz de LEDs com base na umidade
void atualizar_matriz_leds(uint16_t umidade) {
    npClear();
    int leds_acesos = (umidade * LED_COUNT) / 4095;  // Calcula quantos LEDs devem estar acesos

    for (int i = 0; i < leds_acesos; i++) {
        npSetLED(i, 0, 0, 50);  
    }
    npWrite();
}

bool modoManual = false;
uint16_t umidade_simulada = 3000;  // Valor inicial da umidade simulada

// Função para debounce do botão
bool botao_pressionado(uint pin) {
    if (gpio_get(pin) == 0) {  // Botão pressionado (LOW)
        sleep_ms(100);  // Debounce
        if (gpio_get(pin) == 0) {  // Confirmação
            while (gpio_get(pin) == 0);  // Espera soltar o botão
            return true;
        }
    }
    return false;
}

// Função para tocar o buzzer com uma frequência e duração específicas
void tocar_buzzer(uint frequencia, uint duracao_ms) {
    uint periodo_us = 1000000 / frequencia;  // Converte frequência para período em microssegundos
    uint tempo_final = to_ms_since_boot(get_absolute_time()) + duracao_ms;

    while (to_ms_since_boot(get_absolute_time()) < tempo_final) {
        gpio_put(PINO_BUZZER, 1);
        sleep_us(periodo_us / 2);
        gpio_put(PINO_BUZZER, 0);
        sleep_us(periodo_us / 2);
    }
}

// Função para alerta de baixa umidade
void alerta_baixa_umidade() {
    gpio_put(PINO_LED_VERMELHO, 1);  // Liga o LED vermelho
    tocar_buzzer(1000, 500);  // Toca o buzzer com 1000 Hz por 500 ms
    gpio_put(PINO_LED_VERMELHO, 0);  // Desliga o LED vermelho
}

// Função para alerta de umidade alta
void alerta_umidade_alta() {
    gpio_put(PINO_LED_VERMELHO, 1);  // Liga o LED vermelho
    gpio_put(PINO_LED_AZUL, 1);  // Liga o LED azul
    tocar_buzzer(1500, 300);  // Toca o buzzer com 1500 Hz por 300 ms
    sleep_ms(100);
    tocar_buzzer(1500, 300);  // Toca o buzzer novamente
    gpio_put(PINO_LED_VERMELHO, 0);  // Desliga o LED vermelho
    gpio_put(PINO_LED_AZUL, 0);  // Desliga o LED azul
}

// Função para alerta de mudança de modo
void alerta_mudanca_modo() {
    tocar_buzzer(2000, 200);  // Toca o buzzer com 2000 Hz por 200 ms
    sleep_ms(100);
    tocar_buzzer(2000, 200);  // Toca o buzzer novamente
}

int main() {
    stdio_init_all();

    // Inicializa GPIOs
    gpio_init(PINO_RELE);
    gpio_set_dir(PINO_RELE, GPIO_OUT);
    gpio_init(PINO_LED_VERMELHO);
    gpio_set_dir(PINO_LED_VERMELHO, GPIO_OUT);
    gpio_init(PINO_LED_AZUL);
    gpio_set_dir(PINO_LED_AZUL, GPIO_OUT);
    gpio_init(PINO_BUZZER);
    gpio_set_dir(PINO_BUZZER, GPIO_OUT);
    gpio_init(BOTAO_A);
    gpio_set_dir(BOTAO_A, GPIO_IN);
    gpio_pull_up(BOTAO_A);  // Ativa pull-up interno
    gpio_init(BOTAO_MODO);
    gpio_set_dir(BOTAO_MODO, GPIO_IN);
    gpio_pull_up(BOTAO_MODO);  // Ativa pull-up interno

    // Inicializa ADC para o eixo Y do joystick
    adc_init();
    adc_gpio_init(JOYSTICK_Y_PIN);
    adc_select_input(1);  // Seleciona o canal ADC1 (eixo Y)

    // Inicializa I2C e OLED
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    ssd1306_t ssd; // Inicializa a estrutura do display
    ssd1306_init(&ssd, 128, 64, false, endereco, I2C_PORT); // Inicializa o display
    ssd1306_config(&ssd); // Configura o display
    ssd1306_send_data(&ssd); // Envia os dados para o display

    // Inicializa a matriz de LEDs
    npInit(LED_PIN);
    npClear();
    npWrite();

    // Limpa o display
    ssd1306_fill(&ssd, false);
    ssd1306_send_data(&ssd);

    while (true) {
        // Lê o valor do eixo Y do joystick e mapeia para a umidade simulada
        uint16_t joystick_y = adc_read();
        umidade_simulada = (4095 - joystick_y);  // Inverte o valor para simular a umidade

        // Atualiza a matriz de LEDs com base na umidade
        atualizar_matriz_leds(umidade_simulada);

        // Exibe a umidade simulada no OLED
        ssd1306_fill(&ssd, false); // Limpa o display
        ssd1306_draw_string(&ssd, "Sistema", 15, 6); // Título
        ssd1306_draw_string(&ssd, "Irrigacao", 15, 16); // Título

        if (umidade_simulada < LIMITE_UMIDADE_BAIXA) {
            ssd1306_draw_string(&ssd, "Umidade:", 8, 25); // Umidade baixa
            ssd1306_draw_string(&ssd, "Alta", 8, 35); // Umidade baixa
        } else if (umidade_simulada > LIMITE_UMIDADE_ALTA) {
            ssd1306_draw_string(&ssd, "Umidade:", 8, 25); // Umidade alta
            ssd1306_draw_string(&ssd, "Baixa", 8, 35); // Umidade alta
        } else {
            ssd1306_draw_string(&ssd, "Umidade:", 8, 25); // Umidade moderada
            ssd1306_draw_string(&ssd, "Moderada", 8, 35); // Umidade moderada
        }

        // Verifica se o botão de modo foi pressionado para alternar o modo
        if (botao_pressionado(BOTAO_MODO)) {
            modoManual = !modoManual;
            alerta_mudanca_modo();  // Toca o alerta de mudança de modo
        }

        if (modoManual) {
            // Modo manual: pressionar o botão A ativa a irrigação
            ssd1306_draw_string(&ssd, "Modo manual", 8, 48);
            if (botao_pressionado(BOTAO_A)) {
                ssd1306_draw_string(&ssd, "Irrigando...", 8, 48);
                ssd1306_send_data(&ssd); // Atualiza o display
                gpio_put(PINO_RELE, 1);
                gpio_put(PINO_LED_AZUL, 1);  // Liga o LED azul
                tocar_buzzer(1000, 500);  // Toca o buzzer com 1000 Hz por 500 ms
                sleep_ms(4000);  // Mantém a irrigação ativa por 4 segundos
                gpio_put(PINO_RELE, 0);
                gpio_put(PINO_LED_AZUL, 0);  // Desliga o LED azul
                sleep_ms(1000);  // Mantém a mensagem "Irrigando..." por mais 1 segundo
            }
        } else {
            // Modo automático: ativa a irrigação se a umidade estiver baixa
            if (umidade_simulada < LIMITE_UMIDADE_BAIXA) {

                alerta_umidade_alta();  // Toca o alerta de umidade alta
            } else if (umidade_simulada > LIMITE_UMIDADE_ALTA) {
                gpio_put(PINO_RELE, 1);
                alerta_baixa_umidade();  // Toca o alerta de baixa umidade
                ssd1306_draw_string(&ssd, "Irrigando...", 8, 48);
                ssd1306_send_data(&ssd); // Atualiza o display
                sleep_ms(4000);  // Mantém a irrigação ativa por 4 segundos
                gpio_put(PINO_RELE, 0);
                sleep_ms(1000);  // Mantém a mensagem "Irrigando..." por mais 1 segundo            } else {
                gpio_put(PINO_RELE, 0);
                gpio_put(PINO_LED_VERMELHO, 0);  // Desliga o LED vermelho
                ssd1306_draw_string(&ssd, "Modo automatico", 5, 48);
            }
        }

        ssd1306_send_data(&ssd); // Atualiza o display
        sleep_ms(100);  // Delay menor para melhor responsividade
    }
}