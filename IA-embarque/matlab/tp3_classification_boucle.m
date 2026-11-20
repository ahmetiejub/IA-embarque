%% tp3_classification_boucle.m - ETRS923 TP3, projet AMR & EJUB (classification sur le Cloud)
% Toutes les 20 s :
%   1. lit les mesures de la carte des 7 dernieres heures
%      (canal 3546450 : field1 = T, field2 = RH, field3 = P station)
%   2. calcule les entrees du reseau listees dans features.json (memes formules que Colab
%      et que la carte) : temp, rhum, pres (niveau de la mer, 240 m), spread, et selon le
%      modele les tendances dpres1h/3h/6h, dtemp1h/3h, drhum1h/3h, ou (ancien modele a
%      12 entrees) la position et la date/heure UTC
%   3. classe la meteo avec meteo.onnx -> Neige / Nuageux / Pluie / Soleil
%      (ancien modele a 5 classes : Clair -> Soleil, Brouillard -> Nuageux, comme sur la carte)
%   4. ecrit le resultat dans le canal 3546457 (field1 = numero de classe, field2 = confiance %)
%   5. envoie la classe a la carte par TalkBack
% Arret : Ctrl+C.
% Fichiers necessaires dans le meme dossier MATLAB Drive : meteo.onnx, features.json, labels.json
% (les 3 viennent du notebook Colab, toujours les mettre a jour ensemble).

%% --- A REMPLIR : recopier les cles de l'ancien script (NE PAS les mettre sur GitHub) ---
CANAL_MESURES   = 3546450;
CLE_LECTURE     = "";                   % Read API Key du canal 3546450 ("" si le canal est public)
CANAL_RESULTATS = 3546457;
CLE_ECRITURE    = "XXXXXXXXXXXXXXXX";   % Write API Key du canal 3546457
TALKBACK_ID     = "58137";
TALKBACK_CLE    = "XXXXXXXXXXXXXXXX";   % API Key du TalkBack 58137

%% --- Parametres ---
LAT = 45.65;  LON = 5.86;  ALT = 240;  % Le Bourget-du-Lac (memes valeurs que la carte)
periode = 20;                           % s (ThingSpeak : 15 s mini entre 2 ecritures)

net      = importNetworkFromONNX("meteo.onnx", InputDataFormats="BC");
features = string(jsondecode(fileread("features.json")).features);
classes  = string(jsondecode(fileread("labels.json")));
% Classes affichees : 4, comme sur la carte (regroupement si le modele en a 5)
noms4    = ["Neige", "Nuageux", "Pluie", "Soleil"];
vers4    = containers.Map(["Brouillard", "Clair", "Neige", "Nuageux", "Pluie", "Soleil"], ...
                          ["Nuageux",    "Soleil", "Neige", "Nuageux", "Pluie", "Soleil"]);
fprintf("Reseau charge. Entrees : %s\n", strjoin(features, ", "));
fprintf("Classes : %s. Classification toutes les %d s (Ctrl+C pour arreter)\n", strjoin(classes, ", "), periode);
derniere = NaT;

while true
    % 1. Mesures des 7 dernieres heures (pour les tendances jusqu'a 6 h)
    if CLE_LECTURE == ""
        [d, t] = thingSpeakRead(CANAL_MESURES, 'Fields', [1 2 3], 'NumMinutes', 420);
    else
        [d, t] = thingSpeakRead(CANAL_MESURES, 'Fields', [1 2 3], 'NumMinutes', 420, 'ReadKey', CLE_LECTURE);
    end
    ok = all(~isnan(d), 2);
    d = d(ok, :);  t = t(ok);
    if isempty(d) || (~isnat(derniere) && t(end) <= derniere)
        pause(periode); continue;       % pas de nouvelle mesure
    end
    derniere = t(end);

    % 2. Toutes les entrees possibles, puis celles du modele (ordre de features.json)
    T  = d(:, 1);  RH = d(:, 2);
    P  = niveau_mer(d(:, 3), T, ALT);
    v = struct();
    v.temp   = T(end);
    v.rhum   = RH(end);
    v.pres   = P(end);
    v.spread = T(end) - point_rosee(T(end), RH(end));
    v.dpres1h = tendance(t, P, 1);   v.dpres3h = tendance(t, P, 3);   v.dpres6h = tendance(t, P, 6);
    v.dtemp1h = tendance(t, T, 1);   v.dtemp3h = tendance(t, T, 3);
    v.drhum1h = tendance(t, RH, 1);  v.drhum3h = tendance(t, RH, 3);
    % Entrees de l'ancien modele (12 entrees) : position et date/heure UTC
    u = datetime("now", "TimeZone", "UTC");
    v.lat = LAT;  v.lon = LON;  v.alt = ALT;
    v.doy_sin  = sin(2 * pi * day(u, "dayofyear") / 365.25);
    v.doy_cos  = cos(2 * pi * day(u, "dayofyear") / 365.25);
    v.hour_sin = sin(2 * pi * hour(u) / 24);
    v.hour_cos = cos(2 * pi * hour(u) / 24);
    x = zeros(numel(features), 1, "single");
    for i = 1:numel(features)
        x(i) = v.(features(i));
    end

    % 3. Classification
    tic;
    p = extractdata(predict(net, dlarray(x, "BC")));
    duree_ms = 1000 * toc;
    p4 = zeros(1, 4);                   % regroupement en 4 classes
    for i = 1:numel(classes)
        j = find(noms4 == vers4(char(classes(i))));
        p4(j) = p4(j) + p(i);
    end
    [pmax, k] = max(p4);
    classe = noms4(k);
    fprintf("%s  T=%.1f C  RH=%.0f %%  Pmer=%.1f hPa  dpres3h=%.1f -> %s (%.1f %%) en %.1f ms\n", ...
            string(t(end)), v.temp, v.rhum, v.pres, v.dpres3h, classe, 100 * pmax, duree_ms);

    % 4. Resultat dans le canal 3546457
    try
        thingSpeakWrite(CANAL_RESULTATS, [k, 100 * pmax], 'WriteKey', CLE_ECRITURE);
    catch err
        fprintf("  [ThingSpeak] ecriture refusee : %s\n", err.message);
    end

    % 5. Commande TalkBack pour la carte
    try
        webwrite("https://api.thingspeak.com/talkbacks/" + TALKBACK_ID + "/commands.json", ...
                 'api_key', TALKBACK_CLE, 'command_string', char(classe));
    catch err
        fprintf("  [TalkBack] envoi refuse : %s\n", err.message);
    end

    pause(periode);
end

%% --- Memes formules que Colab et la carte ---
function p = niveau_mer(p_station, T, alt)
    p = p_station .* (1 - 0.0065 * alt ./ (T + 0.0065 * alt + 273.15)) .^ -5.257;
end

function td = point_rosee(T, RH)
    a = 17.62;  b = 243.12;                          % Magnus
    g = log(min(max(RH, 1), 100) / 100) + a * T ./ (b + T);
    td = b * g ./ (a - g);
end

function dv = tendance(t, valeurs, heures)
    % valeur actuelle - valeur il y a "heures" heures (mesure la plus proche),
    % 0 s'il n'y a pas de mesure assez proche (comme sur la carte)
    tol = 30;  if heures <= 1, tol = 15; end
    [e, i] = min(abs(minutes(t(end) - t) - 60 * heures));
    if e <= tol, dv = valeurs(end) - valeurs(i); else, dv = 0; end
end
