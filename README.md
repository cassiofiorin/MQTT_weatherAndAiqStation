# MQTT_weatherAndAiqStation
 
Retomada do projeto.

Inicialmente o projeto apenas imprimia as informações no display e via porta serial. A ideia seria armazenar os dados em banco de dados para consulta e histórico.

Atualmente houve uma mudança no segundo ponto, as informações não serão mais salvas em banco de dados, mas sim enviadas o Home Assistant através do protocolo MQTT.

Quanto ao barômetro, também foram criados alertas com base neste sensor, a ideia é alimentar o Home Assistant, que então envia alertas via Telegram sobre mudanças repentinas do clima.

Outra adição importante foi a inclusão do sensor SPS30 da Sensirion, que mede a densisdade de particulas supensas no ar e o tamanho delas. Indicando o nível de poluíção ambiental, assim como suas consequências para a respiração, com índice de qualidade do ar.

Também há arquivos STEP das caixas de cada sensor, esses arquivos são de modelagem próprios para o projeto, realizados conforme medidas de cada sensor, visando otimização do fluxo de ar e para comportar todos os componentes de maneira segura.

O código e comentários foram reorganizados utilizando a Qwen IA.
