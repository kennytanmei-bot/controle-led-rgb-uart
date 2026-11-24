#!/usr/bin/env python3
"""Envia pacotes de teste para a FRDM-KL25Z (pip install pyserial).

Uso:  python testar_uart.py COM5            (Windows)
      python testar_uart.py /dev/ttyACM0 --crc8
"""
import argparse
import time

import serial

PKT_ID = 0xA5  # mesmo valor de PKT_ID no firmware
LIGAR, DESLIGAR, TOGGLE, PISCA_1S, PISCA_2S = 1, 2, 3, 4, 5


def xor8(data):
    x = 0
    for b in data:
        x ^= b
    return x


def crc8(data, poly=0x07):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("porta")
    ap.add_argument("--crc8", action="store_true", help="firmware compilado com USE_CRC8=1")
    args = ap.parse_args()
    chk = crc8 if args.crc8 else xor8

    def pacote(r, g, b, acao, id_=PKT_ID, check_errado=False):
        corpo = bytes([id_, r, g, b, acao])
        c = chk(corpo) ^ (0xFF if check_errado else 0)
        return corpo + bytes([c])

    testes = [
        ("LIGAR vermelho", pacote(255, 0, 0, LIGAR), 1.5),
        ("LIGAR verde 50%", pacote(0, 128, 0, LIGAR), 1.5),
        ("TOGGLE azul (apaga/acende)", pacote(0, 0, 255, TOGGLE), 1.5),
        ("TOGGLE de novo", pacote(0, 0, 255, TOGGLE), 1.5),
        ("PISCA 1s ciano", pacote(0, 255, 255, PISCA_1S), 4),
        ("PISCA 2s magenta", pacote(255, 0, 255, PISCA_2S), 6),
        ("ID errado (deve ser ignorado)", pacote(255, 255, 255, LIGAR, id_=0x11), 1.5),
        ("CHECK errado (deve ser ignorado)", pacote(255, 255, 255, LIGAR, check_errado=True), 1.5),
        ("Lixo + pacote válido (ressincroniza)", b"\x00\x13\x37" + pacote(255, 128, 0, LIGAR), 1.5),
        ("Pacote truncado (3 bytes)", pacote(255, 0, 0, LIGAR)[:3], 0.3),
        ("Após truncado: pacote válido", pacote(0, 255, 0, LIGAR), 1.5),
        ("DESLIGAR", pacote(0, 0, 0, DESLIGAR), 1),
    ]

    print(f"Checksum: {'CRC-8' if args.crc8 else 'XOR'}")
    rotulos = ["ID", "R", "G", "B", "AÇÃO", "CHECK"]
    larguras = [2, 2, 2, 2, 4, 5]  # largura de cada coluna (hex ocupa 2)
    print(f"{'':42s}    " + " | ".join(r.center(w) for r, w in zip(rotulos, larguras)))
    with serial.Serial(args.porta, 115200, bytesize=8, parity="N", stopbits=1) as s:
        for nome, dados, espera in testes:
            campos = " | ".join(
                f"{b:02x}".center(larguras[i] if i < len(larguras) else 2)
                for i, b in enumerate(dados)
            )
            print(f"{nome:42s} -> {campos}")
            s.write(dados)
            time.sleep(espera)


if __name__ == "__main__":
    main()