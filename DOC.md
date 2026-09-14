# DCC831 TP — Respostas para entrega

Experimento: dois nós **m510** do Cloudlab (Xeon D-1548 de 8 núcleos @ 2.0 GHz,
NIC ConnectX-3 10 GbE), conectados diretamente pela porta privada `eno1d1`,
forçados a compartilhar o mesmo switch físico (mapeamento interswitch
desabilitado). DPDK 20.08, driver mlx4 PMD (driver bifurcado).

* **servidor** (node-0): executa o `icmp-echo` na porta DPDK **1**
  (`eno1d1`, MAC `14:58:d0:58:fe:23`). A porta 0 (`eno1`) fica com o Linux
  para o ssh.
* **cliente** (node-1): Linux puro, `eno1d1` = `192.168.1.2/24`, ARP estático
  `192.168.1.3 -> 14:58:d0:58:fe:23`, gera tráfego com `ping`.

Logs brutos: `results/run1-client-ping.txt`, `results/run1-server-stats.txt`.

---

## Q1 — saída do `ping` no cliente

```
# ping -c 5 192.168.1.3   (a frio)
rtt min/avg/max/mdev = 0.040/0.051/0.078/0.014 ms

# sudo ping -f 192.168.1.3   (~15.7 s)
353174 pacotes transmitidos, 353174 recebidos, 0% de perda, tempo 15737ms
rtt min/avg/max/mdev = 0.007/0.010/0.424/0.005 ms, ipg/ewma 0.044/0.010 ms
```

O servidor DPDK respondeu **353179 / 353179** requisições ICMP echo
(353174 da inundação + 5 da execução a frio) com **0% de perda de pacotes**.
Estatísticas do lado do servidor (baseadas no TSC, 2.0 GHz):

| métrica (por resposta echo)                 | valor          |
|----------------------------------------------|---------------:|
| janela rx_burst→tx_burst, média               | 606 ciclos / **303 ns** |
| janela rx_burst→tx_burst, mínima               | 505 ciclos / 253 ns |
| janela rx_burst→tx_burst, máxima (outlier)     | 488313 ciclos / 244 µs |
| ├─ parsing + montagem (lógica da aplicação), média | 153 ciclos / **77 ns** |
| └─ descritor+doorbell de rx/tx do DPDK, média  | 453 ciclos / **227 ns** |

A execução a frio (`ping -c 5`) tem média de 51 µs porque o processo `ping`
precisa ser reagendado (sai do estado "sleep") a cada um dos primeiros
pacotes; a **média em regime de inundação de 10 µs (mínimo de 7 µs)** é o
round trip representativo em estado estacionário e é o valor usado abaixo.

### Validação de corretude (`tests/probe.py`, `results/run2-probe.txt`)

Um probe em scapy rodado a partir do cliente monta frames crus e verifica as
respostas — **19/19 verificações passam**:

* a resposta é de fato reconstruída, não é um simples reflexo de bytes: tipo
  ICMP invertido de 8→0, **ambos os checksums recalculados e válidos**
  (verificado de forma independente), TTL forçado para 64, id/seq e o
  payload completo preservados, IPs de origem/destino trocados;
* o servidor ainda responde a uma requisição com um checksum ICMP
  propositalmente **errado**, e o checksum da resposta sai correto — prova
  de que ele recalcula, em vez de copiar;
* o reflexo é sem estado (*stateless*) — uma requisição para um IP de
  destino forjado é respondida a partir desse mesmo IP;
* todo frame que não é uma requisição echo é **descartado silenciosamente,
  sem travar**: uma *reply* ICMP echo, uma requisição echo com código ≠ 0,
  uma requisição de timestamp, um pacote UDP, um pacote IP truncado, um
  ethertype que não é IPv4. O contador `other packets dropped` do servidor
  contabiliza todos esses casos.

É de fato a aplicação DPDK respondendo, não o kernel: com o `icmp-echo`
parado, `ping 192.168.1.3` tem 100% de perda (nenhum IP vinculado, nenhum
tratamento de ARP no servidor); apagar a entrada estática de ARP do cliente
também derruba a comunicação, pois o servidor nunca responde a ARP.

---

## Q2 — Tempo no nosso software + DPDK versus o resto do caminho. Como medir/inferir a latência do hardware puro?

### Para onde vão os 10 µs do round trip

```
                                        tempo    fração do RTT
cliente: sendto() -> kernel v4/ICMP -> transmissão mlx4 -> doorbell   ─┐
cabo + switch  cliente -> servidor          ~0.1 µs            │  ~9.5 µs
RX na NIC do servidor: DMA via PCIe até o mbuf + anel do PMD mlx4     │   (~95 %)
>>> nossa janela de TSC abre (rx_burst retornou)                    ─┘
parseia ETH/IP/ICMP, troca MACs+IPs, 2 checksums     0.077 µs    0.8 %
tx_burst do DPDK: monta WQE, refcount do mbuf, doorbell de TX 0.227 µs  2.3 %
>>> nossa janela de TSC fecha (tx_burst retornou)
NIC do servidor serializa o frame, cabo+switch de volta  ~0.1 µs    ~1 %
RX na NIC do cliente -> IRQ/NAPI -> skb -> match no socket, ping lê  ─ (dentro dos 9.5 µs)
```

* **Nosso software + o caminho RX/TX do DPDK (medido, lado do servidor):**
  **303 ns em média** (253 ns mínimo). Isso é **~3%** do round trip de
  10 µs (≈ 3.6% do melhor caso de 7 µs).
* **Tudo o mais** = `RTT − 303 ns` ≈ **9.7 µs (média em inundação)** /
  **6.7 µs (mínimo em inundação)**. Isso é dominado pela **pilha de rede
  Linux do cliente** (caminho de envio + de recebimento, sem bypass de
  kernel do lado do cliente), mais os motores de RX/TX de ambas as NICs +
  DMA via PCIe, mais uma fração desprezível do cabo.
* **O cabo não é o gargalo:** um frame ICMP de 98 bytes (+20 B de
  preâmbulo/IFG) a 10 Gbit/s serializa em **~94 ns** em um sentido, ~0.19 µs
  para o round trip.

Então, nesse link, o round trip do ICMP é **quase inteiramente tempo de
CPU/pilha do lado do cliente**, e nosso servidor DPDK é uma fatia de ~3%
disso.

### Confronto com o baseline de kernel (experimento 5 abaixo)

Substituir o servidor DPDK pelo kernel Linux eleva o RTT de 10 → 17 µs
(média), ou seja, **+7 µs** para uma travessia completa da pilha do lado do
servidor. Isso dá um orçamento consistente para o round trip de 10 µs do
DPDK:

| segmento | ~tempo | como foi obtido |
|---|---:|---|
| software do servidor DPDK + caminho RX/TX | 0.3 µs | medido (TSC) |
| pilha Linux do cliente, ambas as direções + syscalls do `ping` | ~7 µs | delta do baseline de kernel, por simetria |
| 2× motores de RX/TX das NICs + DMA via PCIe | ~2 µs | resto |
| cabo + switch (2×) | ~0.2 µs | serialização de um frame de 98 B a 10 GbE |

### Como medir ou inferir a latência do *hardware de rede puro*

O RTT do cliente ainda mistura o kernel do cliente, as duas NICs e o cabo.
Para isolar só a parte de hardware:

1. **Loopback só em DPDK (sem kernel em nenhum ponto).** Fazer a aplicação
   DPDK transmitir um frame de sondagem na porta 1 e recebê-lo de volta
   (fazendo o link dar a volta pelo switch, ou um loopback físico),
   marcando o tempo com `rte_rdtsc_precise()` logo antes do `tx_burst` e
   logo depois que o frame reaparece no `rx_burst`. O resultado é
   `2·(NIC_tx + cabo + NIC_rx)` com **zero pilha de SO**; metade disso é a
   latência de hardware em um sentido. (Exige um caminho de loop — ver a
   nota de "experimento adicional" no README.)

2. **Instrumentar o PMD mlx4 (dica do enunciado).** Em
   `drivers/net/mlx4/mlx4_rxtx.c`, ler o TSC dentro de `mlx4_rx_burst()` no
   ponto em que o PMD lê a entrada da fila de conclusão (completion queue),
   e guardar isso num dynfield do mbuf. Comparar com o TSC no retorno de
   `rx_burst` isola o custo do PMD/anel de RX da DMA que a CPU não consegue
   observar diretamente; o mesmo em torno do caminho de postagem do WQE de
   TX / CQE isola o custo do `tx_burst` voltado para o hardware.

3. **Timestamps de RX em hardware da ConnectX-3.** Habilitar
   `RTE_ETH_RX_OFFLOAD_TIMESTAMP`; a NIC marca cada frame com seu próprio
   relógio no momento da chegada no cabo. `mbuf_timestamp` versus o TSC no
   retorno de `rx_burst` mede diretamente a DMA de RX + latência do anel, em
   unidades do relógio da NIC.

4. **Delta mesmo-switch vs. entre-switches.** Rodar `ping -f` de novo com
   "allow interswitch mapping" habilitado (um salto extra de switch) e
   subtrair do RTT do mesmo switch — a diferença é o store-and-forward de um
   switch mais um cabo extra, ou seja, a latência de cabo+switch por salto,
   isolada.

5. **Baseline com servidor em kernel (feito — `results/run3-kernel-baseline.txt`).**
   Parou-se a aplicação DPDK, deu-se ao servidor o endereço `192.168.1.3/24`
   em `eno1d1`, rodou-se `ping -f` de novo de forma que o **kernel Linux**
   do servidor respondesse:

   | servidor       | RTT em inundação mín / média | trabalho do lado do servidor |
   |----------------|---------------------|------------------|
   | aplicação DPDK | 7 µs / 10 µs        | 303 ns (medido) |
   | kernel Linux   | 11 µs / 17 µs       | ~4–7 µs (delta do RTT) |

   O RTT sobe **+4 µs (mín) / +7 µs (média)** quando o kernel faz o trabalho
   que a aplicação DPDK fazia. Isso mostra que uma travessia completa da
   pilha do lado do servidor (IRQ/NAPI → `ip_rcv` → `icmp_echo` →
   `icmp_reply` → `ip_output` → `dev_queue_xmit`) custa **~4–7 µs**; o DPDK
   faz o mesmo em **0.3 µs**, ~15–20× menos. Por simetria, o kernel do
   *cliente* contribui com algo em torno de ~7 µs no RTT medido na execução
   com DPDK — e é por isso que "o resto do caminho" na Q2 é dominado pela
   pilha do cliente.

---

## Q3 — Overhead introduzido pela stack de software DPDK

Medido no servidor, por resposta echo (TSC @ 2.0 GHz):

| componente                          | ciclos | ns   | fração |
|--------------------------------------|-------:|-----:|------:|
| parsing + montagem (`make_echo_reply`) | 153    | 77   | 25%  |
| `rx_burst` + `tx_burst` (DPDK)        | 453    | 227  | 75%  |
| **total (rx_burst→tx_burst)**         | 606    | 303  | 100% |

* **Lógica da aplicação (77 ns).** Validação de headers, uma troca de
  `rte_ether_addr`, uma troca de endereço IPv4, `rte_ipv4_cksum` sobre 20 B,
  `rte_raw_cksum` sobre os ~64 bytes da mensagem ICMP. As duas passadas de
  checksum são a maior parte disso.
* **Caminho RX/TX do DPDK (227 ns).** `rte_eth_rx_burst` / `rte_eth_tx_burst`
  são wrappers finos e inlined, mas cada um acessa as filas de conclusão e
  de trabalho do mlx4 na memória do host, gerencia refcounts de mbuf e — a
  parte mais cara — emite uma **escrita MMIO de doorbell** para a NIC no TX
  (~100+ ns sozinha). Esse é o custo irredutível de falar com o hardware a
  partir do espaço de usuário.
* **Ressalva de medição.** As leituras aninhadas de `rte_rdtsc_precise()`
  por pacote, usadas para separar aplicação de DPDK, adicionam ~2 leituras
  serializantes (~20–40 ciclos) que caem no balde do "caminho DPDK", então a
  divisão real está uns poucos ns mais deslocada para o lado da aplicação.

**Versus o kernel (medido — `results/run3-kernel-baseline.txt`).**
Rodar o mesmo eco pelo kernel Linux em vez da aplicação DPDK eleva o RTT em
inundação de **10 → 17 µs** (média) e **7 → 11 µs** (mínimo). O trabalho do
lado do servidor, portanto, vai de **0.3 µs (DPDK)** para **~4–7 µs
(kernel)** — uma **redução de 15–20×**. O caminho do kernel paga por uma
interrupção de hardware, softirq de NAPI, alocação/liberação de `sk_buff`,
demultiplexação de protocolo (`ip_rcv`/`icmp_rcv`) e reentrada em
`ip_output`/`dev_queue_xmit`; o caminho do DPDK paga só por um polling da
fila de conclusão do mlx4 e um doorbell de TX. Ou seja, o DPDK não
*adiciona* overhead aqui — ele *remove* o do kernel, trocando ~5 µs de pilha
por ~230 ns de manuseio de descritor/doorbell.

---

## Q4 — A implementação é ótima? O que poderia ser melhorado?

É eficiente, mas **não é ótima**. Em ordem de benefício esperado:

1. **Checksum ICMP incremental.** Só o byte de tipo muda (8 → 0), então o
   `rte_raw_cksum` completo sobre 64 B pode ser substituído por uma única
   soma-com-carry de 16 bits sobre o campo de checksum
   (`csum += ~htons(0x0800); fold`). Remove a maior parte do custo da lógica
   de aplicação.

2. **Offload de checksum de IPv4 na NIC.** Definir
   `RTE_ETH_TX_OFFLOAD_IPV4_CKSUM` + `mbuf->ol_flags |= PKT_TX_IP_CKSUM` e
   deixar a ConnectX-3 preencher o checksum do header IPv4, eliminando a
   passada de `rte_ipv4_cksum` na CPU. (O ICMP não tem offload de hardware
   nessa NIC — daí o ponto 1.)

3. **Amortização do doorbell de TX.** Sob a inundação, o burst de RX já
   carrega muitos pacotes, então um `tx_burst` por burst já amortiza o
   doorbell; adicionar `rte_eth_tx_buffer` com um limiar explícito de flush
   ajudaria em taxas mais baixas ou irregulares. Fazer prefetch do header do
   próximo mbuf (`rte_prefetch0`) enquanto se monta a resposta atual.

4. **Remover a medição de tempo aninhada por pacote** em execuções de
   produção (manter só para uma passada de calibração) — custa 2 leituras
   serializadas de TSC por pacote.

5. **Isolamento de núcleo.** Iniciar o servidor com `isolcpus`/`nohz_full`
   no núcleo 0 (ou rodar sob `taskset` + `chrt`) para remover os outliers de
   244 µs do scheduler vistos no `max`.

6. **NUMA.** Já está correto aqui (socket único); a aplicação avisa se a
   porta estiver em um nó remoto.

7. **Múltiplas filas / RSS + mais lcores** — folga pura de throughput. Um
   único núcleo do m510 sustentou a inundação (22 kpps aqui, e suportaria
   muito mais), então isso não é necessário para o eco ICMP, mas é o
   caminho para taxa de linha.

**Visão geral.** Nosso servidor já é apenas ~3% do round trip. O custo
dominante é a **pilha Linux do cliente**; a mudança de maior impacto para a
latência fim-a-fim seria fazer bypass do kernel também no *cliente* (um
gerador de pacotes em DPDK), ou reduzir a latência de hardware/cabo —
nenhuma das duas é uma mudança neste código. Dentro deste código, os pontos
1 + 2 são os ganhos claros.
