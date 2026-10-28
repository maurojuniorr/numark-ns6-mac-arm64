# Serviço persistente para o transporte USB da NS6

## Problema e evidência

O plug-in HAL roda dentro de helpers do CoreAudio que podem ser substituídos
durante mudanças de clientes. Na captura de 15/10/2026, o helper 52245 recebeu
`kIOReturnExclusiveAccess` ao tentar abrir a interface 0 enquanto o helper
50517 ainda era o dono. O launchd encerrou 50517 às 00:19:08.778; 554 ms depois,
às 00:19:09.332, o novo helper registrou o clock USB em standby.

O estado USB atual é local ao processo. Cada HAL pode iniciar seu próprio
worker; `StopIO` pausa a saída e mantém o worker/interfaces abertos. Isso
permite que um helper novo tente assumir uma interface ainda pertencente ao
helper anterior. A evidência confirma a disputa de propriedade e sua relação
temporal com a troca do helper. O log não mede eletricamente o LED; a piscada
continua sendo correlacionada à interrupção/reabertura do transporte.

## Objetivo

Manter um único dono das interfaces USB da NS6 enquanto o macOS substitui
helpers do CoreAudio, preservando o standby silencioso e evitando erros de
abertura concorrente. O teste final deve também confirmar que a retomada do
áudio, MIDI e captura de entrada continuam funcionais.

## Arquitetura proposta

Transformar o atual processo persistente `NumarkNS6MIDIBridge` em um serviço de
dispositivo NS6 por usuário, executado como LaunchAgent com `KeepAlive`. Ele
será o único componente que abre as interfaces USB e mantém os transports de
áudio, feedback, captura e MIDI. O plug-in HAL deixará de chamar IOKit USB
diretamente.

O HAL e o serviço estabelecerão uma conexão Mach autorizada pela chave
`AudioServerPlugIn_MachServices` do plug-in. O áudio entre os processos usará
buffers compartilhados pré-alocados, com anéis SPSC separados para saída e
entrada. O callback `DoIOOperation` do HAL apenas fará cópias/transferências
limitadas nesses anéis; não fará chamadas XPC síncronas, esperas, alocações ou
operações de arquivo. O serviço consumirá a saída e preencherá a entrada no
ritmo do transporte USB.

O serviço preservará as fontes/destinos CoreMIDI atualmente publicados pelo
bridge. A ativação da NS6 ocorrerá uma única vez no serviço, e as mensagens MIDI
continuarão no fluxo já validado. O HAL usará a mesma conexão persistente para
descobrir estado, informar início/parada de clientes e obter telemetria do
transporte.

## Ciclo de vida

- Ao iniciar sessão, o LaunchAgent inicia o serviço e ele procura a NS6.
- Ao conectar a controladora, o serviço abre as interfaces uma vez, faz a
  inicialização completa e mantém o clock USB em silêncio quando não há áudio.
- Cada instância do HAL conecta-se ao serviço sem abrir as interfaces. Se o
  helper morrer ou for substituído, os anéis daquela conexão são invalidados;
  o serviço USB permanece aberto e a próxima instância cria uma nova sessão.
- Ao desconectar a NS6, o serviço encerra as transferências e libera as
  interfaces. Ao reconectar, inicializa novamente.
- Ao logout ou encerramento do serviço, o transporte é encerrado de forma
  ordenada; não se promete standby fora da sessão do usuário.

## Falhas e recuperação

O serviço é a autoridade única para iniciar, parar e recuperar transferências.
Ele aplicará recuperação limitada por endpoint e informará ao HAL quando o
dispositivo estiver indisponível. Se o serviço cair, o `KeepAlive` poderá
reiniciá-lo; a interrupção nesse caso é esperada e deve ser registrada. A
conexão HAL deve tolerar serviço ainda não iniciado, desconexão, reconexão e
reinício, sem bloquear o callback de áudio.

Buffers compartilhados terão índices atômicos e capacidade fixa. Sobrefluxo ou
subfluxo será contado e registrado; o serviço enviará silêncio quando não
houver frames de saída suficientes e o HAL devolverá silêncio quando a
captura ainda não tiver dados. A retomada usará o prebuffer/fade-out já
validado, sem introduzir um fade-in novo.

## Implantação em etapas

1. Criar o serviço e testar sua inicialização, propriedade exclusiva das
   interfaces, ativação, MIDI e standby sem alterar o caminho HAL atual.
2. Adicionar o protocolo Mach e os anéis compartilhados, primeiro com testes
   de processo substituído e produtor/consumidor sem hardware.
3. Migrar saída de áudio para o serviço e medir callback, underruns, clock e
   latência antes de migrar captura.
4. Migrar captura de áudio e validar MIDI simultâneo.
5. Empacotar como versão experimental separada; manter o pacote estável atual
   disponível para reversão até passar a validação de hardware.

## Critérios de aceite

- Em logins normais, existe no máximo um processo dono das interfaces USB 0 e
  1; trocar/criar helpers HAL não produz `kIOReturnExclusiveAccess`.
- Abrir, pausar e fechar Opera e Mixxx não fecha nem reabre o transporte USB.
- O LED USB permanece estável durante essas transições, confirmado por
  observação humana e correlacionado com logs de transporte contínuo.
- Áudio de saída continua sem clicks/pipocos, sem novos underruns e com clock
  estável em teste prolongado.
- Entrada de áudio e MIDI continuam operacionais simultaneamente com a saída.
- Serviço parado, controlador desconectado e reinício do helper resultam em
  recuperação observável, sem travar CoreAudio nem Mixxx.

## Riscos e decisões pendentes

- A conexão e a memória compartilhada precisam respeitar o sandbox do host
  CoreAudio; validar a chave Mach Services e a transferência de direitos antes
  de migrar o fluxo de áudio.
- A camada de anéis entre processos precisa ter latência e carga de CPU medidas
  no hardware. A segurança de tempo real prevalece sobre conveniência de IPC.
- O processo atual do bridge é um LaunchAgent por usuário. Isso preserva o
  comportamento após login, mas não inicia antes da sessão do usuário.
- O serviço deve assumir todos os endpoints que exigem propriedade exclusiva;
  manter MIDI ou captura USB no HAL poderia recriar a disputa.
- Não alterar o pacote estável até o protótipo validar os critérios de aceite.

## Fora de escopo

- Alterar o mapeamento Mixxx, o driver Linux ou o protocolo MIDI já validado.
- Trocar a taxa de amostragem, o algoritmo de clock adaptativo ou o tamanho de
  buffer configurado pelo aplicativo.
- Modificar a interface do NS6 Status além do necessário para mostrar o estado
  do serviço persistente.
