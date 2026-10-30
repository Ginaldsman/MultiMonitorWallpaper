# Papel de parede por monitor para Windhawk

Módulo local para Windows 11 que aplica imagens diferentes em até três monitores e mantém **o mesmo trio de imagens em todas as áreas de trabalho virtuais**. Ele usa `IDesktopWallpaper`, sem hooks no Explorer, sem gerenciadores externos e sem criar uma imagem panorâmica.

## Arquivos

- `multi-monitor-wallpaper.wh.cpp`: código-fonte completo do módulo.
- `README.md`: instalação, configuração, diagnóstico, limitações e testes.

## Requisitos

- Windows 11 Pro.
- Windhawk 1.7.3 ou posterior. O código também é compatível com os controles de configuração do Windhawk 2.0.
- Três imagens armazenadas em unidades locais, com caminhos absolutos como `C:\Imagens\monitor1.jpg`.

## Instalação como módulo local

1. Abra o Windhawk e ative o modo de desenvolvedor nas configurações, se ele ainda não estiver ativo.
2. Clique em **Create a new mod** / **Criar um novo módulo**.
3. No editor, abra `mod.wh.cpp`. Apague todo o modelo e cole o conteúdo de `multi-monitor-wallpaper.wh.cpp`.
4. Clique em **Compile Mod** ou pressione `Ctrl+B` quando o painel do Windhawk estiver visível.
5. Corrija qualquer erro apresentado pelo compilador antes de continuar. O módulo usa Clang em modo C++23; não é necessário instalar Visual Studio nem outro compilador.
6. Saia do modo de edição e ative o módulo **Papel de parede por monitor**.
7. Configure as imagens conforme a próxima seção. Não é necessário encerrar nem reiniciar o Explorer para instalar o módulo.

O arquivo é carregado somente em `explorer.exe`. Processos secundários do Explorer são ignorados; somente o processo que hospeda a área de trabalho executa o gerenciamento.

## Configuração

| Opção | Função |
|---|---|
| **Gerenciar papéis de parede** | Ativa ou interrompe o gerenciamento. Ao desativar, as imagens atualmente exibidas permanecem como estão. |
| **Entrada 1/2/3 - imagem** | Caminho local completo da imagem de cada entrada. |
| **Entrada 1/2/3 - device path** | Identificador estável e opcional do monitor. É a forma recomendada de associação. |
| **Intervalo de verificação** | Intervalo do fallback para detectar alterações feitas pelo Explorer. Padrão: `2000` ms; o código limita o valor entre `1000` e `60000` ms. |
| **Registrar diagnóstico** | Registra inventário de monitores, identificadores, coordenadas, imagens atuais e associações no log do Windhawk. |

No Windhawk 2.0, os campos de imagem possuem botão para selecionar arquivos. No Windhawk 1.7.3, os mesmos campos aparecem como texto; digite ou cole o caminho completo. Aspas ao redor do caminho e espaços nas extremidades são removidos pelo módulo.

Somente caminhos absolutos de unidades locais, como `C:\Imagens\fundo.jpg`, são aceitos. Caminhos UNC/de rede são recusados para cumprir a restrição de não fazer comunicação de rede.

### Como identificar cada monitor

1. Deixe as três imagens em branco temporariamente, se ainda não houver configuração.
2. Ative **Registrar diagnóstico** e salve as configurações.
3. Na página do módulo, abra **Advanced** / **Avançado**, ative os logs detalhados e depois **Show log output** / **Mostrar log**.
4. Procure linhas semelhantes a:

   ```text
   Monitor index=0, ativo=1, rect=(0,0)-(2560,1440), devicePath="...", wallpaper="...", entrada=1
   ```

5. Use `rect` e a disposição mostrada em **Configurações do Windows > Sistema > Tela** para reconhecer cada monitor. O valor `index` é a posição retornada por `IDesktopWallpaper`; ele não precisa coincidir com o número mostrado pelo Windows.
6. Copie o `devicePath` completo, sem as aspas, para a entrada desejada e informe a imagem dessa entrada.
7. Salve as configurações e confirme a associação no log. Depois, o diagnóstico pode ser desativado.

Se o `device path` ficar vazio, a entrada será associada pela posição retornada por `IDesktopWallpaper`. Esse fallback é conveniente para a configuração inicial, mas **não garante estabilidade** quando monitores são desconectados ou o layout muda. Monitores desconectados são ignorados sem apagar as configurações.

## Como funciona

- Uma thread de trabalho inicializa COM em STA e executa todas as operações demoradas fora da thread principal do Explorer.
- `GetMonitorDevicePathCount`, `GetMonitorDevicePathAt` e `GetMonitorRECT` enumeram os monitores e distinguem os que estão desconectados.
- O módulo procura primeiro uma entrada com o mesmo `device path`. Somente entradas sem identificador usam o fallback por posição.
- `GetWallpaper` é consultado antes de `SetWallpaper`; uma imagem já correta não é reaplicada.
- Mudanças de configuração, vídeo, retorno de suspensão, logon, desbloqueio e reconexão da sessão passam por um debounce de 750 ms.
- Como a API pública de áreas de trabalho virtuais não oferece notificação de troca, uma verificação periódica leve detecta quando o Explorer restaura outra imagem.
- O módulo não cria serviço, processo auxiliar ou instalador, não baixa arquivos, não acessa a rede e não altera a tela de bloqueio.

Alterar a imagem de uma entrada faz com que somente monitores cuja imagem atual seja diferente sejam atualizados. Enquanto o gerenciamento estiver ativo, alterações manuais de wallpaper podem ser substituídas na próxima verificação.

## Diagnóstico e solução de problemas

- **Nenhuma imagem é aplicada:** confirme que o módulo está ativo, que o gerenciamento está ligado e que os caminhos são locais, absolutos e apontam para arquivos existentes.
- **Uma entrada não encontra o monitor:** ative o diagnóstico e compare o `device path` salvo com o identificador atual. Corrija a associação se o hardware tiver mudado.
- **Imagem antiga aparece ao trocar de desktop:** aguarde o intervalo de verificação e o debounce. Se o problema persistir, colete o log antes e depois da troca.
- **Erros de COM:** o módulo registra o `HRESULT`, evita exceções não tratadas e tenta recriar o objeto em verificações posteriores.
- **Inventário não aparece:** desative e reative **Registrar diagnóstico**, salve e verifique se os logs detalhados do módulo estão habilitados.

Para coletar um diagnóstico útil, registre a versão do Windows e do Windhawk, o layout dos monitores, as linhas `Monitor index=...` e o momento aproximado da troca de área de trabalho ou reconexão.

## Limitações do MVP

- Não há trios diferentes para Work, Docs e Chat; todas usam as mesmas três imagens.
- A troca de área de trabalho pode exibir temporariamente a imagem restaurada pelo Windows, por até aproximadamente o intervalo configurado mais o debounce.
- Não há suporte a slideshow, seleção do modo de ajuste, tela de bloqueio ou mais de três entradas.
- A associação por posição pode mudar com o layout; use `device path` para persistência previsível.
- O módulo é injetado no Explorer conforme o escopo solicitado. Embora não use hooks, erros em qualquer módulo injetado podem afetar o processo hospedeiro.
- Desativar ou descarregar o módulo não restaura o wallpaper anterior e não apaga configurações, temas ou imagens.

## Testes manuais

Execute estes testes no computador de destino, com os três `device path` configurados. Nenhum desses testes funcionais é presumido como já executado.

1. **Aplicação inicial**
   - Configure três imagens visualmente distintas e salve.
   - Esperado: cada monitor recebe a imagem da entrada associada, sem alterar a tela de bloqueio.

2. **Alternância entre Work, Docs e Chat**
   - Alterne repetidamente entre as três áreas de trabalho, inclusive rapidamente.
   - Esperado: as áreas de trabalho continuam usáveis e, após o intervalo/debounce, cada monitor conserva a imagem configurada.

3. **Sem reaplicação contínua**
   - Permaneça alguns minutos no mesmo desktop com o diagnóstico ativado.
   - Esperado: não surgem novas mensagens de aplicação enquanto as imagens estão corretas e não há luta visível com o Explorer.

4. **Alteração de uma entrada**
   - Troque somente a imagem da entrada 2.
   - Esperado: somente o monitor associado à entrada 2 muda; os demais permanecem intactos.

5. **Desativar e reativar**
   - Desative o gerenciamento, altere manualmente um wallpaper e aguarde dois intervalos.
   - Esperado: o módulo não desfaz a alteração. Ao reativar, a configuração do módulo volta a ser aplicada.

6. **Bloquear e desbloquear**
   - Pressione `Win+L`, aguarde alguns segundos e desbloqueie.
   - Esperado: o trio é preservado ou restaurado, sem mudança na imagem da tela de bloqueio.

7. **Logoff e login**
   - Faça logoff e entre novamente na mesma conta.
   - Esperado: após o Windhawk e o Explorer iniciarem, o trio configurado é restaurado.

8. **Reinicialização do Windows**
   - Reinicie o computador e entre na mesma conta.
   - Esperado: o trio configurado é restaurado automaticamente, sem aplicar tema manualmente.

9. **Reinicialização do Explorer**
   - Reinicie apenas o Explorer pelo Gerenciador de Tarefas.
   - Esperado: o módulo é carregado novamente e restaura o trio; nenhum comando adicional é necessário.

10. **Desconexão e reconexão**
    - Desconecte um monitor, aguarde e reconecte-o.
    - Esperado: o Explorer permanece estável, as configurações não são apagadas e o monitor reconectado recupera sua imagem pelo `device path`.

11. **Caminho inválido**
    - Informe um arquivo inexistente em uma entrada e salve.
    - Esperado: o módulo registra o problema uma vez, não altera esse monitor e continua tratando os demais sem falhar.

12. **Uso de recursos**
    - Observe o Explorer no Gerenciador de Tarefas durante dez minutos de uso normal e várias trocas de desktop.
    - Esperado: não há uso elevado e contínuo de CPU nem crescimento contínuo evidente de handles ou memória atribuível ao módulo.

## Referências técnicas

- [Windhawk: criação, metadados, configurações, callbacks e compilação](https://github.com/ramensoftware/windhawk/wiki/creating-a-new-mod)
- [Windhawk: ciclo de vida do módulo](https://github.com/ramensoftware/windhawk/wiki/Mod-lifetime)
- [Windhawk: modo de edição](https://github.com/ramensoftware/windhawk/wiki/Editing-mode-guidelines)
- [Microsoft: `IDesktopWallpaper`](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-idesktopwallpaper)
- [Microsoft: `SetWallpaper`](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-idesktopwallpaper-setwallpaper)
- [Microsoft: `GetMonitorDevicePathAt`](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-idesktopwallpaper-getmonitordevicepathat)
- [Microsoft: `GetMonitorRECT`](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-idesktopwallpaper-getmonitorrect)
- [Microsoft: `IVirtualDesktopManager`](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ivirtualdesktopmanager)
