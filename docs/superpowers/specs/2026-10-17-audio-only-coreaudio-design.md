# Driver de saída NS6: desenho de uma base mínima

## Objetivo

Criar uma implementação de saída de áudio para a Numark NS6 original em Apple Silicon/macOS, reduzida ao caminho necessário para o CoreAudio entregar áudio à controladora. Esta primeira etapa não inclui entrada/captura, MIDI, LED, aplicativo Status ou integração com o Mixxx.

O resultado funcional esperado é um dispositivo CoreAudio de quatro canais a 44,1 kHz que aceita clientes simultâneos, mantém os fluxos restantes ao iniciar ou encerrar um cliente e transmite áudio contínuo à NS6. O relógio USB deve seguir o feedback medido da NS6, sem acumular desvio em relação ao fluxo do host.

## Decisões e limites

- Alvo: NS6 original, VID `15e4`, PID `0079`, Apple Silicon e macOS Sequoia.
- O dispositivo anuncia somente saída de quatro canais, 44.1 kHz. CoreAudio recebe PCM float; o transporte converte para o formato USB S24 de quatro canais já validado.
- A implementação de HAL segue o modelo `AudioServerPlugIn` da Apple: operações e buffers são tratados conforme `WillDoIOOperation`/`DoIOOperation`; o driver não cria um fluxo USB por aplicativo.
- O callback de tempo real não faz I/O USB, alocação ou espera bloqueante. Ele entrega os frames do ciclo a uma fila limitada. Um worker único é dono do transporte USB e agenda a saída.
- `StartIO` e `StopIO` são contabilizados por cliente. O transporte não é parado ao sair um cliente enquanto outro ainda mantém I/O ativo. Quando nenhum cliente está ativo, a primeira versão pode encerrar o transporte normalmente.
- O caminho USB de saída continua usando o endpoint de áudio `0x02` e o feedback `0x81`, com o escalonamento de frames inteiros e o controlador adaptativo já comprovados em hardware. A conversão/adaptação respeita o limite de taxa validado e não inventa correções temporais adicionais.
- Só permanecem transações de ativação USB que sejam comprovadamente necessárias ao áudio. Não se cria nem publica qualquer endpoint CoreMIDI e não se lê nem envia tráfego de controles/LED.
- A instalação de teste contém somente o plug-in HAL. A infraestrutura de distribuição, Status, bridge, entrada e recuperação avançada ficam fora desta entrega.

## Ciclo de áudio

1. O CoreAudio inicializa o dispositivo e apresenta um stream de saída de quatro canais, 44.1 kHz.
2. `StartIO(client)` registra cada cliente ativo; o primeiro cliente abre/inicia o transporte e prepara a fila de saída.
3. Nas operações de saída anunciadas pelo driver, o HAL recebe os buffers e o tempo do ciclo. O desenho deve usar a etapa de mixagem definida pelo SDK para que o buffer enviado ao hardware represente o mix de todos os clientes, em vez de empilhar buffers de clientes independentes no FIFO USB.
4. O callback copia o bloco pronto para a fila limitada e retorna sem esperar pelo USB. Se a fila não aceitar um bloco, o driver registra o evento e mantém a resposta do HAL bem-sucedida; não derruba a árvore de áudio do sistema por rejeição local.
5. O worker consome a fila, converte float para S24, agenda pacotes no endpoint `0x02` de acordo com a taxa adaptativa do feedback `0x81` e mantém o pipeline serializado.
6. `StopIO(client)` remove somente aquele cliente. O transporte continua enquanto houver pelo menos um cliente ativo e pode parar quando o último cliente encerrar I/O.

O contrato exato entre `ProcessMix`, `ConvertMix` e `WriteMix`, incluindo buffers in-place e o identificador de cliente, será conferido diretamente no `AudioServerPlugIn.h` do SDK e no exemplo mínimo da Apple antes de implementar as operações. Essa conferência evita assumir que cada chamada de saída já contém o mix final.

## Relógio adaptativo

O fluxo CoreAudio continua nominalmente em 44.1 kHz. O transporte acompanha o relógio físico da NS6 com as medições do endpoint `0x81`; o escalonador distribui frames inteiros por pacote, e o conversor mantém a continuidade entre os ciclos do host e o ritmo USB observado. A fila é limitada e expõe contagem de overflow/underrun para diagnóstico. Nenhum mecanismo de captura, MIDI ou realinhamento por heurística entra nesta etapa.

## Escopo explicitamente adiado

- Entrada de áudio e microfone/line-in.
- MIDI input/output, mensagens de estado inicial e controle dos LEDs.
- Status app, firmware, UI de configuração ou preferências próprias de buffer.
- Serviço persistente, Mach IPC, shared memory, LaunchAgent/LaunchDaemon e reabertura automática após reinício do CoreAudio.
- Taxas diferentes de 44.1 kHz, áudio agregado e instalação de release assinada/notarizada.

## Verificação

- Testes pequenos de regressão para o escalonador de frames por feedback, conversão S24 e fila entre callback e worker; incluir um teste que simule dois clientes e encerre um enquanto o outro continua.
- Compilar o plug-in arm64 e validar que o descritor publicado tem uma saída de quatro canais, 44.1 kHz, sem stream de entrada.
- Inspecionar `WillDoIOOperation`/`DoIOOperation` contra o SDK e garantir que nenhuma chamada USB ou espera bloqueante ocorra no callback.
- Só depois do build e dos testes locais, instalar manualmente o pacote de teste e verificar na NS6: reprodução de tom/canais 1–4, dois clientes simultâneos, fechamento de cada cliente em ordens diferentes, e comportamento ao parar o último cliente.
- A aceitação de hardware exige escuta e observação dos contadores de fila, feedback, erro USB e interrupção do LED; compilação local, sozinha, não comprova estabilidade física.

## Referências

- Apple, [Creating an Audio Server Driver Plug-in](https://developer.apple.com/documentation/coreaudio/creating-an-audio-server-driver-plug-in): exemplo mínimo de plug-in HAL.
- Apple, [`AudioServerPlugInDriverInterface.DoIOOperation`](https://developer.apple.com/documentation/coreaudio/audioserverplugindriverinterface/doiooperation): parâmetros de operação, stream, cliente, tamanho e buffers.
- Apple, [`AudioServerPlugInDriverInterface.StartIO`](https://developer.apple.com/documentation/coreaudio/audioserverplugindriverinterface/startio): o dispositivo considera I/O ativo enquanto ao menos um cliente estiver ativo.
- Apple SDK, `CoreAudio.framework/Headers/AudioServerPlugIn.h`: semântica de `ProcessMix`, `ConvertMix` e `WriteMix` verificada no SDK macOS 26.2 instalado.
