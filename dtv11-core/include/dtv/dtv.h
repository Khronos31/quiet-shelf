/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 公開 API（L3）の C のヘッダの案。dtv-design.hpp を、下の「写す規則」で機械的に写したもの。
 * 決定の経緯は l3/decisions.md にある。
 *
 * 写す規則:
 *   - class X           → 中身を見せない構造体 dtv_x。メソッドは関数 dtv_<動詞>_<目的語>(dtv_x *, …)（決定 20）。
 *                          目的語は、その関数が働きかけるもの（例：open → dtv_open_enclosure、close → dtv_close_stream）。
 *   - get_ のメソッド    → dtv_get_<値の名前>(dtv_x *)。同じ名前の値が複数の対象にあるときだけ、対象を入れる
 *                          （例：dtv_get_enclosure_serial・dtv_get_device_serial）。err を取らず、値をそのまま返す。
 *   - 例外 dtv::Error    → 戻り値（int32_t。dtv_error_code。成功は 0）と、末尾の dtv_error *err（NULL 可）。
 *                          戻り値は err->code と同じ（決定 6）。
 *                          例外：release、get_、設定の関数（dtv_set_*）、失敗しえない関数（dtv_list_models・dtv_get_api_version）は
 *                          err を取らない。close・close_status は状態（dtv_close_state）を返し、止める処理の失敗を err に入れる。
 *   - C++ の戻り値        → err の前に置く出力の引数（out）。ただし一覧と配列を返す関数は、配列をそのまま返す。
 *   - enum class         → 受け渡しは uint32_t。値は C の enum で定める。
 *   - std::optional<bool>→ uint32_t。値は DTV_UNKNOWN・DTV_NO・DTV_YES。
 *   - std::optional<T>   → NULL を入れられるポインタ。
 *   - X *const *（NULL で終わる配列）→ そのまま dtv_x *const *。
 *   - 戻り値の span（ライブラリが持つ byte 列）→ const uint8_t ** と uint64_t * の出力。次に同じものを呼ぶまで有効。
 *   - 引数の span（呼ぶ側の領域）→ ポインタと大きさ。書いた長さは出力で返す。
 *   - bool               → uint32_t（0 か 1）。
 *   - 既定値のある引数    → NULL を渡せば既定値。
 *   - 設定・結果の構造体   → 先頭に struct_size。呼ぶ側が書く設定は呼ぶ側が、ライブラリが書く結果はライブラリが入れる。
 *   - namespace dtv の関数 → dtv_<動詞>_<目的語>（例：list_models → dtv_list_models）。namespace dtv::asicen の関数 → dtv_<名前>_asicen。
 *
 * 決まり（dtv-design.hpp の先頭と同じ。ここでは要点だけ）:
 *   - グローバル変数は使わない。公開するものは、すべて関数（決定 1）。
 *   - dtv_list_enclosures() は、どの thread から呼んでも同じアドレスの、固定長の配列を返す（決定 18）。
 *     枠の数は dtv_get_list_capacity()。空いた枠は NULL で、抜かれた筐体の枠も NULL になる。後ろへ詰めないので、ほかの筐体の枠は動かない。
 *     配列の中身も筐体の値も、USB の状態に合わせて変わる。配列は解放しない（決定 2）。
 *   - dtv_enclosure・dtv_device・dtv_receiver は、プロセス内で共通。1つの接続につき1つで、どの thread から見ても同じアドレス。
 *     製品別の一覧（dtv_list_enclosures_asicen など）と、_by_fds・_by_tokens の一覧も、同じオブジェクトを返す。
 *     挿し直した機器は新しいオブジェクトになる。dtv_connect_enclosure で得た筐体は、つなぐたびに別のオブジェクト。
 *   - 筐体の寿命（決定 14）：抜かれた後も持ち続けるときは dtv_hold_enclosure、要らなくなったら dtv_drop_enclosure。
 *     開いている筐体は release するまで hold しているものとみなす。抜かれていて誰も hold せず開いてもいない筐体は、
 *     次に（どの thread であれ）一覧の関数か dtv_wait_event が呼ばれたときに解放する。
 *     hold していないポインタは、それまで有効。長く使うときや別の thread に渡すときは hold する。
 *   - 共通の入口が扱うのは、dtv_load_<製品>() を呼んだ製品だけ（決定 17）。最初に一覧を取る前に呼ぶ。
 *   - 層（決定 19）：操作の公開の名前は dtv_<function>()・dtv_<function>_<product>()・dtv_<function>_<product>_<model>() の3つ。
 *     Core 関数と製品関数は、判定と振り分けだけをする。機種関数が実装で、製品の中の共通の処理（非公開）を呼んでもよい。
 *     機種関数を直接呼んだときは、Core 関数と製品関数の判定・筐体の操作の順番待ちを通らない。その安全は呼ぶ側の責任。
 *   - 値は get_ の関数で読む。get_ はその時点の値を返す。
 *   - 一覧の扱いは、呼ぶ側が順番をつける（決定 47）。次の3つは、同時に重ねない。同じ thread で行うか、呼ぶ側の lock で守る。
 *     ・一覧の関数（dtv_list_enclosures 系、_by_fds・_by_tokens、dtv_list_ungrouped_devices）と dtv_wait_event を呼ぶこと
 *     ・一覧の配列を読むこと
 *     ・配列から読んだ筐体を hold すること
 *     hold した筐体（と、その中の device・receiver）への get_ と操作は、どの thread から呼んでもよい。
 *   - USB は libusb に依存する。
 *   - ライブラリが中で作る thread は、開いている筐体ごとに1つ（既決）。dtv_open_enclosure() で作り、dtv_close_enclosure() で止める。
 *     開いていない機器の変化は、dtv_wait_event() を呼んだ thread の上で見る（決定 10）。
 *   - thread から呼ぶときの決まり・閉じる期限・token・open に失敗したときの回収は、dtv-design.hpp の先頭のとおり。
 *   - 機器ごとの状態は台ごとに分ける。プロセス全体に効くこと（mlockall・スケジューリングの変更・シグナル・
 *     exit/abort・標準エラーへの出力）をしない。ログは dtv_set_log_callback() の callback に出す。
 *   - メモリは確保した側が解放する。DLL の境界で CRT の資源や C++ の型を渡さない。callback には C の型だけを使う（§10.7）。
 *   - 名前と値は、正本の --list-json（docs/json-discovery.md）に揃える。--list-json の項目は、すべて公開 API で読める。
 */
#ifndef DTV_DTV_H
#define DTV_DTV_H

#include <stdint.h>

/* 筐体の一覧の枠の数（決定 18）。ライブラリをビルドするときに -D で変えられる。
 * 読む側は、この値ではなく dtv_get_list_capacity() を使う（ライブラリとアプリで値が違っても読み違えないため） */
#ifndef DTV_MAX_ENCLOSURES
#define DTV_MAX_ENCLOSURES 128
#endif

struct libusb_device;                      /* libusb の型。名前だけを使う（libusb.h は読み込まない） */

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== エラー ===================== */
enum dtv_error_code {
    DTV_ERROR_OK = 0, DTV_ERROR_INVALID_ARGUMENT = 1, DTV_ERROR_NOT_FOUND = 2, DTV_ERROR_NOT_READY = 3,
    DTV_ERROR_UNSUPPORTED = 4, DTV_ERROR_BUSY = 5, DTV_ERROR_TIMEOUT = 6, DTV_ERROR_VERSION_MISMATCH = 7,
    DTV_ERROR_PROTOCOL_ERROR = 8, DTV_ERROR_USB_IO = 9, DTV_ERROR_DISCONNECTED = 10, DTV_ERROR_TS_ERROR = 11,
    DTV_ERROR_NO_CARD = 12, DTV_ERROR_CARD_REMOVED = 13, DTV_ERROR_CARD_IO = 14, DTV_ERROR_BUFFER_TOO_SMALL = 15,
    DTV_ERROR_FIRMWARE_REJECTED = 16, DTV_ERROR_PERMISSION_DENIED = 17, DTV_ERROR_RESOURCE_LIMIT = 18,
    DTV_ERROR_OUTPUT_ERROR = 19, DTV_ERROR_INTERNAL = 20,
    DTV_ERROR_CANCELLED = 21,              /* cancel や close で止めた */
    DTV_ERROR_SLOW_CONSUMER = 22           /* 読む側が遅く、TS を渡しきれなかった（daemon につないだ筐体） */
};
enum dtv_error_domain { DTV_ERROR_DOMAIN_NONE = 0, DTV_ERROR_DOMAIN_LIBUSB = 1, DTV_ERROR_DOMAIN_OS = 2,
                        DTV_ERROR_DOMAIN_CARD = 3 };
typedef struct dtv_error {
    uint32_t struct_size;                  /* 呼ぶ側が入れる */
    int32_t  code;                         /* dtv_error_code */
    uint32_t cause_domain;                 /* dtv_error_domain */
    int32_t  cause_code;                   /* libusb のエラー番号、errno など。なければ 0 */
    char    *message;                      /* 呼ぶ側が用意した領域。NULL なら書かない。
                                              ライブラリは NUL 終端で書き、入りきらなければ切り詰める。
                                              中身は診断のための参考で、約束にしない（分岐には code を使う）。
                                              製品のライブラリが入れた文字列も、Core を通してここに入る */
    uint64_t message_capacity;             /* message の領域の大きさ（byte。NUL を含む） */
} dtv_error;

/* CLI の終了コードとの対応（決定 30。外部契約 3.7 の分類に当てはめたもの）:
 *   0：OK
 *   2：INVALID_ARGUMENT
 *   3：NOT_FOUND・NOT_READY・UNSUPPORTED
 *   4：BUSY
 *   5：TIMEOUT
 *   6：VERSION_MISMATCH・PROTOCOL_ERROR
 *   7：USB_IO・DISCONNECTED
 *   8：TS_ERROR・SLOW_CONSUMER
 *   9：NO_CARD・CARD_REMOVED・CARD_IO、カードの関数で起きた BUFFER_TOO_SMALL
 *  10：FIRMWARE_REJECTED
 *  70：PERMISSION_DENIED・RESOURCE_LIMIT・OUTPUT_ERROR・INTERNAL、カードの関数以外で起きた BUFFER_TOO_SMALL、
 *      CLI 自身が止めたのではない CANCELLED（CLI 自身が SIGINT などで止めたときの CANCELLED は 0） */

/* ===================== 値の種類 ===================== */
enum dtv_tribool { DTV_UNKNOWN = 0, DTV_NO = 1, DTV_YES = 2 };   /* std::optional<bool>（JSON の null / false / true） */
enum dtv_system { DTV_SYSTEM_ISDB_T = 1, DTV_SYSTEM_ISDB_S = 2 };
enum dtv_lnb_state { DTV_LNB_UNKNOWN = 0, DTV_LNB_OFF = 1, DTV_LNB_ON = 2 };
enum dtv_card_state { DTV_CARD_UNKNOWN = 0, DTV_CARD_ABSENT = 1, DTV_CARD_PRESENT = 2, DTV_CARD_READY = 3 };   /* READY は ATR が取れた状態 */
enum dtv_enclosure_status {                /* JSON の enclosures[].status */
    DTV_ENCLOSURE_UNKNOWN = 0, DTV_ENCLOSURE_READY = 1,   /* 2（INCOMPLETE）は欠番（決定 44） */
    DTV_ENCLOSURE_DUPLICATE = 3, DTV_ENCLOSURE_INVALID_OBSERVATION = 4,
    DTV_ENCLOSURE_DISCONNECTED = 5         /* 抜かれた。一覧からは消えている */
};
enum dtv_device_mode {                     /* JSON には無い。ファームの転送と、抜けたときのため */
    DTV_DEVICE_UNKNOWN = 0, DTV_DEVICE_LOADER = 1, DTV_DEVICE_RUNTIME = 2,
    DTV_DEVICE_DISCONNECTED = 5            /* 3・4 は欠番（決定 38 で WAITING_DEVICE・OTHER_PRODUCT をなくした） */
};
enum dtv_ungrouped_status {                /* JSON の ungrouped_usb_devices[].status */
    DTV_UNGROUPED_UNKNOWN = 0, DTV_UNGROUPED_USABLE = 1, DTV_UNGROUPED_UNSUPPORTED = 2,
    DTV_UNGROUPED_INVALID_SERIAL = 3, DTV_UNGROUPED_INSUFFICIENT_SPEED = 4,
    DTV_UNGROUPED_INVALID_TOPOLOGY = 5, DTV_UNGROUPED_OPEN_FAILED = 6
};
enum dtv_ts_scrambling {                   /* 受信部が出す TS */
    DTV_TS_UNKNOWN = 0,
    DTV_TS_SCRAMBLED = 1,                  /* スクランブルされた生の TS。B25 は利用者が行う */
    DTV_TS_DESCRAMBLED = 2                 /* 機器の中で解いたもの（PIX の公式ファーム。AES は製品のライブラリが中で解く） */
};
enum dtv_close_state {
    DTV_CLOSE_CLOSED = 1,                  /* 止まりきった。release してよい */
    DTV_CLOSE_PENDING = 2                  /* 期限が切れた。遅れて届く完了を回収中。close_status で確かめ直す */
};
enum dtv_operation {                       /* 能力を聞くときの操作の種類 */
    DTV_OP_LNB_POWER = 1, DTV_OP_CARD = 2, DTV_OP_OUTPUT_PID_FILTER = 3, DTV_OP_TUNE_ISDB_T = 4,
    DTV_OP_TUNE_ISDB_S = 5, DTV_OP_SELECT_SLOT = 6, DTV_OP_SELECT_TSID = 7, DTV_OP_SIGNAL = 8, DTV_OP_LNA = 9
};
enum dtv_tune_selector { DTV_TUNE_SELECTOR_NONE = 0, DTV_TUNE_SELECTOR_SLOT = 1, DTV_TUNE_SELECTOR_TSID = 2 };
enum dtv_lna { DTV_LNA_DEFAULT = 0, DTV_LNA_OFF = 1, DTV_LNA_ON = 2 };   /* 受信の増幅器。DEFAULT は機種の既定 */

enum dtv_event_kind {                      /* 知らせる変化の種類（要件 F-16） */
    DTV_EVENT_USB_CONNECTED = 1, DTV_EVENT_USB_DISCONNECTED = 2, DTV_EVENT_CARD_INSERTED = 3,
    DTV_EVENT_CARD_REMOVED = 4, DTV_EVENT_LOCK_ACQUIRED = 5, DTV_EVENT_LOCK_LOST = 6
};

/* ===================== 引数・結果 ===================== */
typedef struct dtv_call_options {          /* 待つ操作の期限。NULL を渡せば既定の期限 */
    uint32_t struct_size;
    uint64_t timeout_ms;                   /* 0 は期限なし */
} dtv_call_options;
typedef void (*dtv_log_callback)(void *user, uint32_t level, const char *message);
typedef uint64_t dtv_usb_token;            /* 選択済みの機器を指す、中身の見えない値。0 は無効。発行と失効は呼ぶ側が持つ */
typedef dtv_usb_token (*dtv_token_of_callback)(void *user, struct libusb_device *dev);   /* その機器の token。知らなければ 0 */
typedef struct dtv_tune_options {          /* ≒ --slot / --tsid */
    uint32_t struct_size;
    uint32_t selector;                     /* dtv_tune_selector */
    uint32_t value;                        /* 機種の範囲外の slot は UNSUPPORTED（決定 48）。TSID を選べるかは DTV_OP_SELECT_TSID で聞く */
    uint32_t lna;                          /* dtv_lna */
} dtv_tune_options;
enum dtv_measurement_state { DTV_MEASUREMENT_UNKNOWN = 0, DTV_MEASUREMENT_UNSUPPORTED = 1, DTV_MEASUREMENT_VALID = 2 };
typedef struct dtv_measurement {         /* wire の測定に揃える */
    uint32_t state;                        /* dtv_measurement_state */
    uint32_t reason;                       /* UNKNOWN・UNSUPPORTED の理由（wire の reason） */
    int32_t  value;                        /* VALID のときだけ意味がある */
} dtv_measurement;
typedef struct dtv_signal {
    uint32_t struct_size;                  /* ライブラリが入れる */
    dtv_measurement lock;                  /* value は 0 か 1 */
    dtv_measurement cnr_mdb;               /* C/N。m dB。校正していない生の値は出さない（UNSUPPORTED） */
    dtv_measurement signal_mdbm;           /* 信号の電力。m dBm。AGC や強度を名前だけ当てはめない */
} dtv_signal;
typedef struct dtv_serve_options {         /* ≒ *d の option */
    uint32_t struct_size;
    const char *runtime_dir;
    const char *instance;
    uint32_t flags;
} dtv_serve_options;
typedef struct dtv_connect_options {       /* ≒ *-ts / *ctl の option */
    uint32_t struct_size;
    const char *runtime_dir;
    const char *instance;
} dtv_connect_options;
typedef struct dtv_model {                 /* ≒ --models の1行 */
    const char *product;
    const char *name;
    uint16_t vid;
    uint16_t pid;
} dtv_model;

/* ===================== オブジェクト（中身を見せない） ===================== */
typedef struct dtv_enclosure dtv_enclosure;   /* JSON の enclosures[] */
typedef struct dtv_device dtv_device;         /* JSON の enclosures[].devices[]（USB の function） */
typedef struct dtv_receiver dtv_receiver;     /* JSON の devices[].receivers[] */
typedef struct dtv_stream dtv_stream;         /* ≒ *-ts の出力 */

/* ===================== 値だけの構造体（作られた後は変わらない） ===================== */
typedef struct dtv_candidate {             /* JSON の enclosures[].candidates[] */
    const uint32_t struct_size;
    const uint32_t device;                 /* 旧来のデバイスの slot 番号 */
    const char *const serial;              /* NULL 可 */
    const uint32_t *const bus;             /* NULL 可 */
    const uint32_t *const address;         /* NULL 可 */
    const char *const port;                /* NULL 可 */
} dtv_candidate;
typedef struct dtv_ungrouped_device {      /* JSON の ungrouped_usb_devices[] */
    const uint32_t struct_size;
    const char *const serial;              /* NULL 可 */
    const char *const model;               /* NULL 可 */
    const char *const port;                /* NULL 可 */
    const uint16_t vid, pid;
    const uint32_t *const bus;             /* NULL 可 */
    const uint32_t *const address;         /* NULL 可 */
    const uint32_t status;                 /* dtv_ungrouped_status */
} dtv_ungrouped_device;
typedef struct dtv_event {                 /* 決定 13。知らせは受け取られるまでライブラリが溜める。数に上限は置かない */
    uint32_t struct_size;                  /* ライブラリが入れる */
    uint32_t kind;                         /* dtv_event_kind */
    dtv_enclosure *enclosure;              /* その変化が起きた筐体。新規接続のときは、一覧に新しく現れたもの。寿命は筐体の決まりに従う */
    dtv_receiver *receiver;                /* ロックの変化のときの受信部。それ以外は NULL */
} dtv_event;

/* ===================== 全体 ===================== */
uint32_t dtv_get_api_version(void);
void     dtv_set_log_callback(dtv_log_callback cb, void *user);       /* プロセスに1回。最初に一覧を取る前に呼ぶ（決定 11） */
void     dtv_set_token_of(dtv_token_of_callback cb, void *user);      /* 同上。WebUSB の token の経路でだけ使う */

const dtv_model *const *dtv_list_models(void);   /* NULL で終わる。リンクした全製品の機種 ≒ --models */
int32_t dtv_get_product_support(const char *product, uint32_t operation, uint32_t *out_tribool, dtv_error *err);   /* 決定 7 */
int32_t dtv_get_model_support(const char *model, uint32_t operation, uint32_t *out_tribool, dtv_error *err);       /* 決定 7 */

/* 一覧の配列は dtv_get_list_capacity() 個の枠。空いた枠は NULL。どの thread からでも同じアドレス（決定 2・18）。失敗は NULL。
 * 枠を超えた機器は一覧に出さず、そのことをログに出す。製品別・_by_fds・_by_tokens の一覧も同じ形 */
uint32_t dtv_get_list_capacity(void);
dtv_enclosure *const *dtv_list_enclosures(dtv_error *err);   /* 機種が確定し、USB device が全部揃った筐体だけを並べる（決定 40・44） */
dtv_enclosure *const *dtv_list_enclosures_by_fds(const int32_t *fds, uint32_t fd_count, dtv_error *err);   /* Android */
/* WebUSB：選択済みの機器（token）から一覧を作る。L3 は libusb の一覧の機器ごとに token_of で token を聞き、一致した機器だけを使う。
 * token_of が設定されていなければ INVALID_ARGUMENT。一致しない token（失効したものなど）は NOT_FOUND で、別の機器は開かない */
dtv_enclosure *const *dtv_list_enclosures_by_tokens(const dtv_usb_token *tokens, uint32_t token_count, dtv_error *err);
/* 製品が宣言した VID:PID の device のうち、揃った筐体にならなかったもの。機種として宣言していない VID:PID
 * （ASICEN の AUSBDTV など）は、ここにも出ない（決定 45）。
 * NULL で終わる。--list-json の ungrouped_usb_devices。要素は1つの接続につき1つで、見えなくなったら筐体と同じ時に解放する。
 * 中身は変わらない。観測した値が変われば新しい要素になる（決定 15） */
const dtv_ungrouped_device *const *dtv_list_ungrouped_devices(void);
uint32_t dtv_get_scan_complete(void);                                 /* --list-json の scan.complete（0 か 1） */

/* daemon につなぐ ≒ *-ts / *ctl。where は serial か USB path。返る筐体は開いた状態。
 * 閉じた後は dtv_release_enclosure でオブジェクトごと解放する */
int32_t dtv_connect_enclosure(const char *where, const dtv_connect_options *, const dtv_call_options *,
                              dtv_enclosure **out, dtv_error *err);

/* 接続されているすべての device（筐体ではなく）に、既定のファームを送る（決定 40）。
 * 頭から順に見て、ファームが要る device に焼く。いなくなっている device は飛ばす。
 * 焼いた後に runtime で現れ直した device が、相方を出すための後処理（ASICEN の sibling restore など）を要るなら、
 * その device を短く開いて後処理をする（決定 41）。少し待って、頭から見直す。
 * ファームが要る device も、後処理が要る device もなくなったら成功で戻る。ほかが掴んでいる device は飛ばし、最後まで焼けなかったものがあればエラーを返す。
 * 全体の期限（call の timeout_ms）が来たら TIMEOUT を返す。Android・WebUSB での振る舞いは未決 */
int32_t dtv_send_default_firmware_to_all(const dtv_call_options *, dtv_error *err);

/* 操作していない間に起きた変化を待つ（決定 10）。変化が起きるか期限が来るまで戻らない。期限が来たら TIMEOUT。
 * daemon につないだ筐体の変化も届く（決定 27・34）。ただし USB の新規接続は、daemon からは届かない */
int32_t dtv_wait_event(dtv_event *out, const dtv_call_options *, dtv_error *err);

/* ===================== dtv_enclosure ===================== */
uint32_t       dtv_get_enclosure_id(dtv_enclosure *);
const char    *dtv_get_product(dtv_enclosure *);        /* "asicen" など。製品は一覧に現れた時点で決まっている（決定 29） */
const char    *dtv_get_model(dtv_enclosure *);          /* 機種が決まるまでは NULL。一度決まったら変わらない */
const char    *dtv_get_enclosure_serial(dtv_enclosure *);         /* NULL 可 */
uint32_t       dtv_get_serial_unique(dtv_enclosure *);  /* dtv_tribool */
uint32_t       dtv_get_status(dtv_enclosure *);         /* dtv_enclosure_status。抜かれたら DISCONNECTED */
uint32_t       dtv_get_lnb_state(dtv_enclosure *);      /* dtv_lnb_state。給電の能力は dtv_get_enclosure_support(DTV_OP_LNB_POWER) で聞く */
uint32_t       dtv_get_card_state(dtv_enclosure *);     /* dtv_card_state。カードリーダを扱えるかは dtv_get_enclosure_support(DTV_OP_CARD) で聞く */
uint32_t       dtv_get_card_generation(dtv_enclosure *);   /* ライブラリが観測した抜き差しの回数。取りこぼしはありうる */
dtv_device    *dtv_get_card_reader_device(dtv_enclosure *);   /* 無い・分からない間は NULL */
dtv_device *const *dtv_get_devices(dtv_enclosure *);           /* NULL で終わる */
const dtv_candidate *const *dtv_get_candidates(dtv_enclosure *);   /* NULL で終わる。要素は筐体が残っている間有効（決定 15） */

int32_t  dtv_get_enclosure_support(dtv_enclosure *, uint32_t operation, uint32_t *out_tribool, dtv_error *err);
int32_t  dtv_open_enclosure(dtv_enclosure *, const dtv_call_options *, dtv_error *err);   /* 物理的に USB を掴む（claim まで）。他が掴んでいれば BUSY */
uint32_t dtv_close_enclosure(dtv_enclosure *, const dtv_call_options *, dtv_error *err);   /* dtv_close_state を返す。中の thread も止める */
uint32_t dtv_get_enclosure_close_state(dtv_enclosure *, dtv_error *err);                     /* dtv_close_state */
void     dtv_release_enclosure(dtv_enclosure *);   /* CLOSED の後で、残った資源を最後に解放する */
void     dtv_hold_enclosure(dtv_enclosure *);      /* 抜かれた後も持ち続ける（決定 14） */
void     dtv_drop_enclosure(dtv_enclosure *);      /* hold をやめる */
/* ≒ *ctl lnb-on / lnb-off。筐体ごとに入れる・切る（bridge ごとなどの扱いはライブラリの中で閉じる）。
 * 直接使うときは、dtv_lnb_on を呼ぶこと自体を給電の許可とみなす。
 * daemon では、今までどおり起動時の許可（dtv_serve_options の flags。≒ --allow-lnb-power）に従う */
int32_t  dtv_lnb_on(dtv_enclosure *, const dtv_call_options *, dtv_error *err);
int32_t  dtv_lnb_off(dtv_enclosure *, const dtv_call_options *, dtv_error *err);
/* カード ≒ *ctl card-atr / card-reset / card-apdu。dtv_get_enclosure_support(DTV_OP_CARD) が DTV_YES のときだけ使える（それ以外は UNSUPPORTED）。
 * 返る byte 列はライブラリが持ち、次にこの筐体のカードの関数を呼ぶまで有効 */
int32_t  dtv_get_card_atr(dtv_enclosure *, const dtv_call_options *,
                                    const uint8_t **out_data, uint64_t *out_size, dtv_error *err);
int32_t  dtv_reset_card(dtv_enclosure *, const dtv_call_options *,
                                  const uint8_t **out_atr, uint64_t *out_size, dtv_error *err);
int32_t  dtv_transmit_apdu(dtv_enclosure *, const uint8_t *command, uint64_t command_size,
                                          const dtv_call_options *,
                                          const uint8_t **out_response, uint64_t *out_size, dtv_error *err);
int32_t  dtv_serve_enclosure(dtv_enclosure *, const dtv_serve_options *, dtv_error *err);   /* daemon として動かす ≒ *d */
int32_t  dtv_cancel_enclosure(dtv_enclosure *, dtv_error *err);   /* 別の thread から呼んでよい */

/* ===================== dtv_device ===================== */
uint32_t        dtv_get_device_id(dtv_device *);
uint16_t        dtv_get_vid(dtv_device *);         /* JSON の usb */
uint16_t        dtv_get_pid(dtv_device *);
const char     *dtv_get_device_serial(dtv_device *);      /* NULL 可 */
const uint32_t *dtv_get_bus(dtv_device *);         /* NULL 可 */
const uint32_t *dtv_get_address(dtv_device *);     /* NULL 可 */
const char     *dtv_get_port(dtv_device *);        /* NULL 可 */
uint32_t        dtv_get_mode(dtv_device *);        /* dtv_device_mode */
dtv_receiver *const *dtv_get_receivers(dtv_device *);   /* NULL で終わる。分からない間は NULL。0 個と分かれば先頭が NULL の配列 */

/* loader なら送る。その製品が使えるファームで既に runtime なら、何もせず成功。別の方式のファームで runtime なら、載せ直す
 * （どちらに当たるかは製品が判断する）。mode が UNKNOWN なら、I/O を出さずに NOT_READY を返す。
 * ファームを送ると USB に現れ直す機種（再列挙する機種）では、書き込んだところで戻る。その筐体は抜かれたのと同じ扱いになり、
 * 現れ直した機器は、別の新しい筐体として一覧に出る（決定 38）。利用者は一覧を取り直す。
 * 同じ USB の接続のまま進む機種では、runtime で見分け直すまで待って戻る。同じ筐体のまま値が変わる。
 * 1回に渡すファームは、そのデバイスに要る image を全部まとめたもの（package）。製品が決めた順に書き込む。
 * 渡されたファームの中身は、ライブラリは確かめない。確かめるのは渡す側の責任（決定 32） */
int32_t dtv_send_firmware(dtv_device *, const uint8_t *firmware, uint64_t size,
                                 const dtv_call_options *, dtv_error *err);
/* 既定のファーム（製品のライブラリに埋め込んだもの。決定 31）を送る。
 * 全機種が既定のファームを宣言するか、ファーム無しを宣言する（決定 36。宣言が無ければビルドエラー）。
 * ファーム無しの機種では、何もせずに成功する */
int32_t dtv_send_default_firmware(dtv_device *, const dtv_call_options *, dtv_error *err);

/* ===================== dtv_receiver ===================== */
uint32_t dtv_get_index(dtv_receiver *);           /* 筐体の中で一意な番号 ≒ --receiver */
uint32_t dtv_get_local(dtv_receiver *);           /* 親のデバイスの中での番号 */
uint32_t dtv_get_systems(dtv_receiver *);         /* 1 << dtv_system の和 */
uint32_t dtv_get_ts_scrambling(dtv_receiver *);   /* dtv_ts_scrambling。製品の変換の後の、放送の TS の状態（USB の AES は含まない）。
                                                              ファームを載せるまでは UNKNOWN。載せた後は、そのファームの間は変わらない */
int32_t dtv_get_receiver_support(dtv_receiver *, uint32_t operation, uint32_t *out_tribool, dtv_error *err);
int32_t dtv_tune_by_frequency(dtv_receiver *, uint32_t system, uint64_t frequency_hz,
                                       const dtv_tune_options *, const dtv_call_options *, dtv_error *err);
int32_t dtv_tune_by_channel(dtv_receiver *, const char *channel,      /* ≒ --channel */
                                     const dtv_tune_options *, const dtv_call_options *, dtv_error *err);
int32_t dtv_get_signal(dtv_receiver *, const dtv_call_options *, dtv_signal *out, dtv_error *err);
int32_t dtv_open_stream(dtv_receiver *, const dtv_call_options *, dtv_stream **out, dtv_error *err);

/* ===================== dtv_stream ===================== */
uint64_t dtv_get_dropped_bytes(dtv_stream *);   /* 区切り合わせで捨てた byte 数 */
uint64_t dtv_get_desyncs(dtv_stream *);         /* 区切りがずれた回数 */
/* 先頭が 0x47 の 188 byte のパケットの並びだけを返す。区切り合わせは Core が行う。
 * USB から来るパケットの大きさ（188 か 192）と同期の byte の位置（0 か 4）は機種が Core に申告し、
 * Core はその大きさで区切りを合わせてから製品の変換に渡す。
 * 同じ IN に応答や印が混ざる機種で、区切り合わせの前に中身を分ける段が要るかは、TS をつなぐときに Core が決める。
 * out_size は書いた byte 数（188 の倍数）。終わりなら 0 */
int32_t  dtv_read_stream(dtv_stream *, uint8_t *buffer, uint64_t capacity, const dtv_call_options *,
                         uint64_t *out_size, dtv_error *err);
int32_t  dtv_set_pid_filter(dtv_stream *, const uint16_t *pids, uint32_t count, dtv_error *err);
int32_t  dtv_cancel_stream(dtv_stream *, dtv_error *err);   /* 別の thread から呼んでよい */
uint32_t dtv_close_stream(dtv_stream *, const dtv_call_options *, dtv_error *err);   /* dtv_close_state */
uint32_t dtv_get_stream_close_state(dtv_stream *, dtv_error *err);
void     dtv_release_stream(dtv_stream *);   /* CLOSED の後で解放する。これ以降 stream は使えない */

/* ===================== 製品ごとの入口（asicen の例。px4・pix なども同じ形） ===================== */
void dtv_load_asicen(void);   /* 共通の入口でこの製品を扱えるようにする（決定 17）。何度呼んでもよい */
const dtv_model *const *dtv_list_models_asicen(void);
int32_t dtv_get_product_support_asicen(uint32_t operation, uint32_t *out_tribool, dtv_error *err);
int32_t dtv_get_model_support_asicen(const char *model, uint32_t operation, uint32_t *out_tribool, dtv_error *err);
dtv_enclosure *const *dtv_list_enclosures_asicen(dtv_error *err);
/* asicen の device だけに送る。製品が手で書く（決定 43）。中身は製品の自由。決定 40〜42 の決まりと戻り値に従う */
int32_t dtv_send_default_firmware_to_all_asicen(const dtv_call_options *, dtv_error *err);
dtv_enclosure *const *dtv_list_enclosures_by_fds_asicen(const int32_t *fds, uint32_t fd_count, dtv_error *err);
dtv_enclosure *const *dtv_list_enclosures_by_tokens_asicen(const dtv_usb_token *tokens, uint32_t token_count, dtv_error *err);
/* 製品関数（自動生成。判定して機種関数へ振り分ける。決定 19・26・43）。
 * 機種関数 dtv_<function>_asicen_<機種>()（例：dtv_lnb_on_asicen_px_w3u3）も、同じ引数と返り値を持つ。
 * 機種関数は、宣言した機能・ファームについては手で書き、宣言しなかったものは自動で作られる */
int32_t dtv_lnb_on_asicen(dtv_enclosure *, const dtv_call_options *, dtv_error *err);
int32_t dtv_lnb_off_asicen(dtv_enclosure *, const dtv_call_options *, dtv_error *err);
int32_t dtv_tune_by_frequency_asicen(dtv_receiver *, uint32_t system, uint64_t frequency_hz,
                                     const dtv_tune_options *, const dtv_call_options *, dtv_error *err);
int32_t dtv_get_signal_asicen(dtv_receiver *, const dtv_call_options *, dtv_signal *out, dtv_error *err);
int32_t dtv_set_pid_filter_asicen(dtv_stream *, const uint16_t *pids, uint32_t count, dtv_error *err);
int32_t dtv_send_firmware_asicen(dtv_device *, const uint8_t *firmware, uint64_t size,
                                 const dtv_call_options *, dtv_error *err);
int32_t dtv_send_default_firmware_asicen(dtv_device *, const dtv_call_options *, dtv_error *err);
int32_t dtv_connect_enclosure_asicen(const char *where, const dtv_connect_options *, const dtv_call_options *,
                                     dtv_enclosure **out, dtv_error *err);

#ifdef __cplusplus
}
#endif

#endif /* DTV_DTV_H */
