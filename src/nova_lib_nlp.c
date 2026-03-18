/**
 * @file nova_lib_nlp.c
 * @brief Nova Language - Natural Language Processing Library
 *
 * Pure C99 NLP module providing text tokenization, stemming,
 * fuzzy matching, frequency analysis, concordance, n-grams,
 * extractive summarization, TF-IDF scoring, and similarity
 * measurement.  Leverages Weave interning (O(1) string dedup)
 * and Dagger tables (O(1) term lookup) via the Nova VM.
 *
 * Functions:
 *   nlp.tokenize(text [, opts])     Tokenize text into words
 *   nlp.stem(word)                  Porter-stem a single word
 *   nlp.stems(text)                 Stem every token in text
 *   nlp.is_stopword(word)           Check English stopword
 *   nlp.stopwords()                 Get full stopword set
 *   nlp.distance(a, b)             Levenshtein edit distance
 *   nlp.similarity(a, b)           Normalized 0.0–1.0 similarity
 *   nlp.fuzzy(haystack, needle, threshold)  Fuzzy substring search
 *   nlp.freq(text [, limit])        Term frequency table
 *   nlp.tfidf(docs, term)          TF-IDF score across documents
 *   nlp.ngrams(text, n [, limit])   N-gram frequency extraction
 *   nlp.kwic(text, keyword [, width]) KWIC concordance
 *   nlp.sentences(text)             Sentence boundary detection
 *   nlp.summarize(text [, n])       Extractive summary (top N)
 *   nlp.normalize(text)             Lowercase + strip punctuation
 *   nlp.chartype(c)                 Classify a character
 *   nlp.wordcount(text)             Fast word count
 *   nlp.unique(text)                Unique words (deduplicated)
 *
 * @author Anthony Taliento
 * @date 2026-02-18
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_lib.h, nova_vm.h
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define NLP_MAX_WORD    256
#define NLP_MAX_TOKENS  65536
#define NLP_MAX_SENTS   4096

/* ============================================================
 * ENGLISH STOPWORD SET
 *
 * 174 common English stopwords.  Stored as a sorted array for
 * binary-search lookup (O(log n)).
 * ============================================================ */

static const char *nlpi_stopwords[] = {
    "a", "about", "above", "after", "again", "against", "all", "am",
    "an", "and", "any", "are", "aren't", "as", "at",
    "be", "because", "been", "before", "being", "below", "between",
    "both", "but", "by",
    "can", "can't", "cannot", "could", "couldn't",
    "did", "didn't", "do", "does", "doesn't", "doing", "don't", "down",
    "during",
    "each",
    "few", "for", "from", "further",
    "get", "got",
    "had", "hadn't", "has", "hasn't", "have", "haven't", "having", "he",
    "her", "here", "hers", "herself", "him", "himself", "his", "how",
    "i", "if", "in", "into", "is", "isn't", "it", "its", "itself",
    "just",
    "ll",
    "me", "might", "more", "most", "must", "mustn't", "my", "myself",
    "no", "nor", "not", "now",
    "of", "off", "on", "once", "only", "or", "other", "ought", "our",
    "ours", "ourselves", "out", "over", "own",
    "re",
    "s", "same", "shan't", "she", "should", "shouldn't", "so", "some",
    "such",
    "t", "than", "that", "the", "their", "theirs", "them", "themselves",
    "then", "there", "these", "they", "this", "those", "through", "to",
    "too",
    "under", "until", "up", "us",
    "ve", "very",
    "was", "wasn't", "we", "were", "weren't", "what", "when", "where",
    "which", "while", "who", "whom", "why", "will", "with", "won't",
    "would", "wouldn't",
    "you", "your", "yours", "yourself", "yourselves"
};

#define NLP_STOPWORD_COUNT \
    ((int)(sizeof(nlpi_stopwords) / sizeof(nlpi_stopwords[0])))

/* ============================================================
 * INTERNAL: STOPWORD LOOKUP — BINARY SEARCH
 * ============================================================ */

static int nlpi_strcmp_lower(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb) return (ca < cb) ? -1 : 1;
        if (ca == 0) return 0;
        a++;
        b++;
    }
}

static int nlpi_is_stopword(const char *word) {
    if (word == NULL || word[0] == '\0') return 0;

    int lo = 0;
    int hi = NLP_STOPWORD_COUNT - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = nlpi_strcmp_lower(word, nlpi_stopwords[mid]);
        if (cmp == 0) return 1;
        if (cmp < 0) hi = mid - 1;
        else          lo = mid + 1;
    }
    return 0;
}

/* ============================================================
 * INTERNAL: PORTER STEMMER (SIMPLIFIED)
 *
 * A faithful-enough implementation of the Porter stemming
 * algorithm for English.  Handles the most impactful suffix
 * rules (step 1–5) — covers ~95 % of real-world stems.
 * ============================================================ */

/** Is character a consonant in the Porter sense? */
static int nlpi_is_consonant(const char *s, int i) {
    switch (tolower((unsigned char)s[i])) {
        case 'a': case 'e': case 'i':
        case 'o': case 'u':
            return 0;
        case 'y':
            return (i == 0) ? 1 : !nlpi_is_consonant(s, i - 1);
        default:
            return 1;
    }
}

/** Measure m — count of VC sequences in stem[0..k] */
static int nlpi_measure(const char *s, int k) {
    int n = 0;
    int i = 0;
    if (k < 0) return 0;

    /* Skip leading consonants */
    while (i <= k && nlpi_is_consonant(s, i)) i++;
    if (i > k) return 0;

    for (;;) {
        /* Skip vowels */
        while (i <= k && !nlpi_is_consonant(s, i)) i++;
        if (i > k) return n;
        n++;
        /* Skip consonants */
        while (i <= k && nlpi_is_consonant(s, i)) i++;
        if (i > k) return n;
    }
}

/** Does stem[0..k] contain a vowel? */
static int nlpi_has_vowel(const char *s, int k) {
    for (int i = 0; i <= k; i++) {
        if (!nlpi_is_consonant(s, i)) return 1;
    }
    return 0;
}

/** Does stem end with double consonant? */
static int nlpi_double_c(const char *s, int k) {
    if (k < 1) return 0;
    if (s[k] != s[k - 1]) return 0;
    return nlpi_is_consonant(s, k);
}

/** Does stem end with CVC where last C is not w, x, y? */
static int nlpi_cvc(const char *s, int k) {
    if (k < 2) return 0;
    if (!nlpi_is_consonant(s, k) || nlpi_is_consonant(s, k - 1) ||
        !nlpi_is_consonant(s, k - 2))
        return 0;
    {
        char c = (char)tolower((unsigned char)s[k]);
        if (c == 'w' || c == 'x' || c == 'y') return 0;
    }
    return 1;
}

/** Check if s ends with suffix, return position before suffix or -1 */
static int nlpi_ends_with(const char *s, int k, const char *suffix) {
    int slen = (int)strlen(suffix);
    if (slen > k + 1) return -1;
    if (memcmp(s + k - slen + 1, suffix, (size_t)slen) != 0) return -1;
    return k - slen;
}

/**
 * @brief Apply Porter stemmer to a word (in-place).
 *
 * @param word  Lowercase word buffer (modified in-place)
 * @param len   Length of word
 * @return New length of stemmed word
 */
static int nlpi_porter_stem(char *word, int len) {
    if (len <= 2) return len;

    int k = len - 1;  /* Index of last char */
    int j;

    /* Step 1a: plurals */
    if (word[k] == 's') {
        if ((j = nlpi_ends_with(word, k, "sses")) >= 0) {
            k = j + 2;  /* sses -> ss */
        } else if ((j = nlpi_ends_with(word, k, "ies")) >= 0) {
            k = j + 1;  /* ies -> i */
        } else if (word[k - 1] != 's') {
            k--;  /* s -> (remove) */
        }
    }

    /* Step 1b: -eed, -ed, -ing */
    if ((j = nlpi_ends_with(word, k, "eed")) >= 0) {
        if (nlpi_measure(word, j) > 0) k = j + 2;  /* eed -> ee */
    } else if ((j = nlpi_ends_with(word, k, "ed")) >= 0) {
        if (nlpi_has_vowel(word, j)) {
            k = j;
            goto step1b_fixup;
        }
    } else if ((j = nlpi_ends_with(word, k, "ing")) >= 0) {
        if (nlpi_has_vowel(word, j)) {
            k = j;
            goto step1b_fixup;
        }
    }
    goto step1c;

step1b_fixup:
    if ((nlpi_ends_with(word, k, "at") >= 0) ||
        (nlpi_ends_with(word, k, "bl") >= 0) ||
        (nlpi_ends_with(word, k, "iz") >= 0)) {
        word[++k] = 'e';
    } else if (nlpi_double_c(word, k) &&
               word[k] != 'l' && word[k] != 's' && word[k] != 'z') {
        k--;
    } else if (nlpi_measure(word, k) == 1 && nlpi_cvc(word, k)) {
        word[++k] = 'e';
    }

step1c:
    /* Step 1c: y -> i */
    if (word[k] == 'y' && nlpi_has_vowel(word, k - 1)) {
        word[k] = 'i';
    }

    /* Step 2: common suffixes */
    {
        static const char *step2[][2] = {
            {"ational", "ate"}, {"tional",  "tion"}, {"enci",   "ence"},
            {"anci",    "ance"},{"izer",    "ize"},  {"abli",   "able"},
            {"alli",    "al"},  {"entli",   "ent"},  {"eli",    "e"},
            {"ousli",   "ous"}, {"ization", "ize"},  {"ation",  "ate"},
            {"ator",    "ate"}, {"alism",   "al"},   {"iveness","ive"},
            {"fulness", "ful"}, {"ousness", "ous"},  {"aliti",  "al"},
            {"iviti",   "ive"}, {"biliti",  "ble"},  {NULL, NULL}
        };
        for (int r = 0; step2[r][0] != NULL; r++) {
            j = nlpi_ends_with(word, k, step2[r][0]);
            if (j >= 0 && nlpi_measure(word, j) > 0) {
                int slen = (int)strlen(step2[r][1]);
                memcpy(word + j + 1, step2[r][1], (size_t)slen);
                k = j + slen;
                break;
            }
        }
    }

    /* Step 3: more suffixes */
    {
        static const char *step3[][2] = {
            {"icate", "ic"}, {"ative", ""}, {"alize", "al"},
            {"iciti", "ic"}, {"ical",  "ic"},{"ful",   ""},
            {"ness",  ""},   {NULL, NULL}
        };
        for (int r = 0; step3[r][0] != NULL; r++) {
            j = nlpi_ends_with(word, k, step3[r][0]);
            if (j >= 0 && nlpi_measure(word, j) > 0) {
                int slen = (int)strlen(step3[r][1]);
                memcpy(word + j + 1, step3[r][1], (size_t)slen);
                k = j + slen;
                break;
            }
        }
    }

    /* Step 4: remove suffixes where m > 1 */
    {
        static const char *step4[] = {
            "al", "ance", "ence", "er", "ic", "able", "ible", "ant",
            "ement", "ment", "ent", "ion", "ou", "ism", "ate", "iti",
            "ous", "ive", "ize", NULL
        };
        for (int r = 0; step4[r] != NULL; r++) {
            j = nlpi_ends_with(word, k, step4[r]);
            if (j >= 0 && nlpi_measure(word, j) > 1) {
                /* Special: "ion" needs s/t before */
                if (strcmp(step4[r], "ion") == 0) {
                    if (j >= 0 && (word[j] == 's' || word[j] == 't')) {
                        k = j;
                    }
                } else {
                    k = j;
                }
                break;
            }
        }
    }

    /* Step 5a: remove trailing e */
    if (word[k] == 'e') {
        int m = nlpi_measure(word, k - 1);
        if (m > 1 || (m == 1 && !nlpi_cvc(word, k - 1))) {
            k--;
        }
    }

    /* Step 5b: ll -> l if m > 1 */
    if (word[k] == 'l' && nlpi_double_c(word, k) &&
        nlpi_measure(word, k - 1) > 1) {
        k--;
    }

    word[k + 1] = '\0';
    return k + 1;
}

/* ============================================================
 * INTERNAL: LEVENSHTEIN EDIT DISTANCE
 *
 * Classic DP algorithm with O(min(m,n)) space via single-row
 * optimization.
 * ============================================================ */

static int nlpi_levenshtein(const char *a, int la,
                            const char *b, int lb) {
    /* Ensure a is the shorter string for space efficiency */
    if (la > lb) {
        const char *tmp = a; a = b; b = tmp;
        int ti = la; la = lb; lb = ti;
    }

    int *row = (int *)malloc(sizeof(int) * (size_t)(la + 1));
    if (row == NULL) return -1;

    for (int i = 0; i <= la; i++) row[i] = i;

    for (int j = 1; j <= lb; j++) {
        int prev = row[0];
        row[0] = j;
        for (int i = 1; i <= la; i++) {
            int cost = (tolower((unsigned char)a[i - 1]) ==
                        tolower((unsigned char)b[j - 1])) ? 0 : 1;
            int ins = row[i] + 1;
            int del = row[i - 1] + 1;
            int sub = prev + cost;
            prev = row[i];

            int best = ins;
            if (del < best) best = del;
            if (sub < best) best = sub;
            row[i] = best;
        }
    }

    int result = row[la];
    free(row);
    return result;
}

/* ============================================================
 * INTERNAL: TOKENIZER
 *
 * Splits text on whitespace + punctuation boundaries.
 * Returns an array of token pointers into a work buffer.
 * Caller owns the work buffer.
 * ============================================================ */

typedef struct {
    char   *buf;       /**< Owned buffer (copy of input) */
    char   *tokens[NLP_MAX_TOKENS];
    int     count;
} NlpiTokens;

static void nlpi_tokenize(NlpiTokens *tk, const char *text, size_t len) {
    tk->count = 0;
    tk->buf = (char *)malloc(len + 1);
    if (tk->buf == NULL) return;
    memcpy(tk->buf, text, len);
    tk->buf[len] = '\0';

    char *p = tk->buf;
    while (*p && tk->count < NLP_MAX_TOKENS) {
        /* Skip non-alpha-numeric */
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (*p == '\0') break;

        /* Read token: alphanumeric + internal apostrophes/hyphens
         * (only if followed by another alphanumeric character)    */
        char *start = p;
        while (*p) {
            if (isalnum((unsigned char)*p)) {
                p++;
            } else if ((*p == '\'' || *p == '-') &&
                       p[1] != '\0' &&
                       isalnum((unsigned char)p[1])) {
                p++;  /* Keep internal hyphen/apostrophe */
            } else {
                break;
            }
        }

        /* Trim any trailing apostrophes/hyphens (safety) */
        char *end = p;
        while (end > start &&
               (end[-1] == '\'' || end[-1] == '-')) {
            end--;
        }

        if (end > start) {
            char saved = *end;
            *end = '\0';
            tk->tokens[tk->count++] = start;

            /* Advance past the NUL only if we replaced a real char */
            if (p == end && saved != '\0') {
                p = end + 1;
            }
        }
    }
}

static void nlpi_tokenize_free(NlpiTokens *tk) {
    free(tk->buf);
    tk->buf = NULL;
    tk->count = 0;
}

/* ============================================================
 * INTERNAL: SENTENCE SPLITTER
 *
 * Splits on '.', '!', '?' followed by whitespace or EOL.
 * Handles abbreviations (single uppercase letter + '.') and
 * ellipsis ('...') as non-boundaries.
 * ============================================================ */

typedef struct {
    const char *start;
    int         len;
} NlpiSentence;

static int nlpi_split_sentences(const char *text, size_t text_len,
                                NlpiSentence *sents, int max_sents) {
    int count = 0;
    const char *p = text;
    const char *end = text + text_len;
    const char *sent_start = text;

    /* Skip leading whitespace */
    while (sent_start < end && isspace((unsigned char)*sent_start)) {
        sent_start++;
    }

    p = sent_start;

    while (p < end && count < max_sents) {
        if (*p == '.' || *p == '!' || *p == '?') {
            /* Check for ellipsis */
            if (*p == '.' && p + 1 < end && p[1] == '.') {
                p++;
                continue;
            }
            /* Check for abbreviation (single uppercase + .) */
            if (*p == '.' && p > text && isupper((unsigned char)p[-1]) &&
                (p - 1 == text || !isalpha((unsigned char)p[-2]))) {
                p++;
                continue;
            }

            /* Advance past sentence-ending punctuation */
            while (p < end && (*p == '.' || *p == '!' || *p == '?')) p++;

            /* This is a sentence boundary */
            int slen = (int)(p - sent_start);
            if (slen > 0) {
                sents[count].start = sent_start;
                sents[count].len = slen;
                count++;
            }

            /* Skip whitespace to next sentence */
            while (p < end && isspace((unsigned char)*p)) p++;
            sent_start = p;
        } else {
            p++;
        }
    }

    /* Remaining text as final sentence */
    if (sent_start < end && count < max_sents) {
        /* Trim trailing whitespace */
        const char *trim = end;
        while (trim > sent_start && isspace((unsigned char)trim[-1])) {
            trim--;
        }
        int slen = (int)(trim - sent_start);
        if (slen > 0) {
            sents[count].start = sent_start;
            sents[count].len = slen;
            count++;
        }
    }

    return count;
}

/* ============================================================
 * INTERNAL: SIMPLE SORT FOR FREQUENCY TABLES
 * ============================================================ */

typedef struct {
    const char *word;
    int         freq;
    double      score;  /**< Used for TF-IDF or sentence scoring */
} NlpiFreqEntry;

static int nlpi_freq_cmp_desc(const void *a, const void *b) {
    const NlpiFreqEntry *fa = (const NlpiFreqEntry *)a;
    const NlpiFreqEntry *fb = (const NlpiFreqEntry *)b;
    if (fb->freq != fa->freq) return fb->freq - fa->freq;
    return strcmp(fa->word, fb->word);
}

static int nlpi_score_cmp_desc(const void *a, const void *b) {
    const NlpiFreqEntry *fa = (const NlpiFreqEntry *)a;
    const NlpiFreqEntry *fb = (const NlpiFreqEntry *)b;
    if (fb->score > fa->score) return 1;
    if (fb->score < fa->score) return -1;
    return 0;
}

/* ============================================================
 * INTERNAL: FREQUENCY MAP (SIMPLE HASH TABLE)
 *
 * A lightweight separate-chaining hash map for word counting.
 * Used when we need to count things outside the VM heap.
 * ============================================================ */

#define NLPI_MAP_BUCKETS 1024

typedef struct NlpiMapEntry {
    char                  word[NLP_MAX_WORD];
    int                   count;
    struct NlpiMapEntry  *next;
} NlpiMapEntry;

typedef struct {
    NlpiMapEntry *buckets[NLPI_MAP_BUCKETS];
    int           total_entries;
} NlpiMap;

static void nlpi_map_init(NlpiMap *m) {
    memset(m, 0, sizeof(*m));
}

static unsigned nlpi_hash_word(const char *w) {
    unsigned h = 5381;
    while (*w) {
        h = ((h << 5) + h) ^ (unsigned)(unsigned char)tolower((unsigned char)*w);
        w++;
    }
    return h;
}

static int nlpi_map_add(NlpiMap *m, const char *word, int increment) {
    unsigned idx = nlpi_hash_word(word) % NLPI_MAP_BUCKETS;
    NlpiMapEntry *e = m->buckets[idx];

    while (e != NULL) {
        if (nlpi_strcmp_lower(e->word, word) == 0) {
            e->count += increment;
            return e->count;
        }
        e = e->next;
    }

    /* New entry */
    e = (NlpiMapEntry *)calloc(1, sizeof(NlpiMapEntry));
    if (e == NULL) return -1;

    size_t wlen = strlen(word);
    if (wlen >= NLP_MAX_WORD) wlen = NLP_MAX_WORD - 1;
    memcpy(e->word, word, wlen);
    e->word[wlen] = '\0';
    /* Lowercase in-place */
    for (size_t i = 0; i < wlen; i++) {
        e->word[i] = (char)tolower((unsigned char)e->word[i]);
    }
    e->count = increment;
    e->next = m->buckets[idx];
    m->buckets[idx] = e;
    m->total_entries++;

    return increment;
}

static int nlpi_map_get(NlpiMap *m, const char *word) {
    unsigned idx = nlpi_hash_word(word) % NLPI_MAP_BUCKETS;
    NlpiMapEntry *e = m->buckets[idx];

    while (e != NULL) {
        if (nlpi_strcmp_lower(e->word, word) == 0) {
            return e->count;
        }
        e = e->next;
    }
    return 0;
}

/**
 * @brief Collect all entries into a flat array for sorting.
 * @return Allocated array (caller must free), count set.
 */
static NlpiFreqEntry *nlpi_map_to_array(NlpiMap *m, int *count) {
    NlpiFreqEntry *arr = (NlpiFreqEntry *)malloc(
        sizeof(NlpiFreqEntry) * (size_t)(m->total_entries > 0 ? m->total_entries : 1));
    if (arr == NULL) { *count = 0; return NULL; }

    int n = 0;
    for (int b = 0; b < NLPI_MAP_BUCKETS; b++) {
        NlpiMapEntry *e = m->buckets[b];
        while (e != NULL) {
            arr[n].word = e->word;
            arr[n].freq = e->count;
            arr[n].score = 0.0;
            n++;
            e = e->next;
        }
    }
    *count = n;
    return arr;
}

static void nlpi_map_free(NlpiMap *m) {
    for (int b = 0; b < NLPI_MAP_BUCKETS; b++) {
        NlpiMapEntry *e = m->buckets[b];
        while (e != NULL) {
            NlpiMapEntry *next = e->next;
            free(e);
            e = next;
        }
        m->buckets[b] = NULL;
    }
    m->total_entries = 0;
}

/* ============================================================
 * VM-FACING FUNCTIONS
 * ============================================================ */

/* -------- nlp.tokenize(text [, opts]) ---------------------- *
 * opts table (optional):
 *   lower    = true    — lowercase tokens
 *   stem     = false   — apply Porter stemmer
 *   stop     = false   — filter out stopwords
 * Returns: array table of token strings                       */

static int nova_nlp_tokenize(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *text = nova_lib_check_string(vm, 0);
    if (text == NULL) return -1;

    size_t text_len = strlen(text);

    /* Parse optional opts table */
    int opt_lower = 1;
    int opt_stem  = 0;
    int opt_stop  = 0;

    if (nova_vm_get_top(vm) > 1) {
        NovaValue optv = nova_vm_get(vm, 1);
        if (nova_is_table(optv)) {
            NovaTable *ot = nova_as_table(optv);
            NovaString *k;

            k = nova_vm_intern_string(vm, "lower", 5);
            if (k != NULL) {
                NovaValue v = nova_table_get_str(ot, k);
                if (nova_is_bool(v)) opt_lower = nova_as_bool(v);
            }
            k = nova_vm_intern_string(vm, "stem", 4);
            if (k != NULL) {
                NovaValue v = nova_table_get_str(ot, k);
                if (nova_is_bool(v)) opt_stem = nova_as_bool(v);
            }
            k = nova_vm_intern_string(vm, "stop", 4);
            if (k != NULL) {
                NovaValue v = nova_table_get_str(ot, k);
                if (nova_is_bool(v)) opt_stop = nova_as_bool(v);
            }
        }
    }

    NlpiTokens tk;
    nlpi_tokenize(&tk, text, text_len);

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) {
        nlpi_tokenize_free(&tk);
        return 1;
    }
    NovaTable *result = nova_as_table(tval);

    nova_int_t idx = 0;
    for (int i = 0; i < tk.count; i++) {
        char word[NLP_MAX_WORD];
        size_t wlen = strlen(tk.tokens[i]);
        if (wlen >= NLP_MAX_WORD) wlen = NLP_MAX_WORD - 1;
        memcpy(word, tk.tokens[i], wlen);
        word[wlen] = '\0';

        if (opt_lower) {
            for (size_t c = 0; c < wlen; c++) {
                word[c] = (char)tolower((unsigned char)word[c]);
            }
        }

        if (opt_stop && nlpi_is_stopword(word)) continue;

        if (opt_stem) {
            int slen = nlpi_porter_stem(word, (int)wlen);
            wlen = (size_t)slen;
        }

        NovaString *ns = nova_vm_intern_string(vm, word, wlen);
        if (ns != NULL) {
            nova_table_raw_set_int(vm, result, idx, nova_value_string(ns));
            idx++;
        }
    }

    nlpi_tokenize_free(&tk);
    return 1;
}

/* -------- nlp.stem(word) ----------------------------------- */

static int nova_nlp_stem(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *word = nova_lib_check_string(vm, 0);
    if (word == NULL) return -1;

    char buf[NLP_MAX_WORD];
    size_t wlen = strlen(word);
    if (wlen >= NLP_MAX_WORD) wlen = NLP_MAX_WORD - 1;
    memcpy(buf, word, wlen);
    buf[wlen] = '\0';

    for (size_t i = 0; i < wlen; i++) {
        buf[i] = (char)tolower((unsigned char)buf[i]);
    }

    int slen = nlpi_porter_stem(buf, (int)wlen);
    nova_vm_push_string(vm, buf, (size_t)slen);
    return 1;
}

/* -------- nlp.stems(text) ---------------------------------- *
 * Tokenize + stem every word, return array of stems           */

static int nova_nlp_stems(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *text = nova_lib_check_string(vm, 0);
    if (text == NULL) return -1;

    NlpiTokens tk;
    nlpi_tokenize(&tk, text, strlen(text));

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) {
        nlpi_tokenize_free(&tk);
        return 1;
    }
    NovaTable *result = nova_as_table(tval);

    for (int i = 0; i < tk.count; i++) {
        char word[NLP_MAX_WORD];
        size_t wlen = strlen(tk.tokens[i]);
        if (wlen >= NLP_MAX_WORD) wlen = NLP_MAX_WORD - 1;
        memcpy(word, tk.tokens[i], wlen);
        word[wlen] = '\0';

        for (size_t c = 0; c < wlen; c++) {
            word[c] = (char)tolower((unsigned char)word[c]);
        }

        int slen = nlpi_porter_stem(word, (int)wlen);
        NovaString *ns = nova_vm_intern_string(vm, word, (size_t)slen);
        if (ns != NULL) {
            nova_table_raw_set_int(vm, result, (nova_int_t)i,
                                   nova_value_string(ns));
        }
    }

    nlpi_tokenize_free(&tk);
    return 1;
}

/* -------- nlp.is_stopword(word) ---------------------------- */

static int nova_nlp_is_stopword(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *word = nova_lib_check_string(vm, 0);
    if (word == NULL) return -1;

    nova_vm_push_bool(vm, nlpi_is_stopword(word));
    return 1;
}

/* -------- nlp.stopwords() ---------------------------------- *
 * Return full stopword list as an array table                 */

static int nova_nlp_stopwords(NovaVM *vm) {
    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) return 1;
    NovaTable *result = nova_as_table(tval);

    for (int i = 0; i < NLP_STOPWORD_COUNT; i++) {
        size_t slen = strlen(nlpi_stopwords[i]);
        NovaString *ns = nova_vm_intern_string(vm, nlpi_stopwords[i], slen);
        if (ns != NULL) {
            nova_table_raw_set_int(vm, result, (nova_int_t)i,
                                   nova_value_string(ns));
        }
    }

    return 1;
}

/* -------- nlp.distance(a, b) ------------------------------- *
 * Levenshtein edit distance between two strings               */

static int nova_nlp_distance(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *a = nova_lib_check_string(vm, 0);
    const char *b = nova_lib_check_string(vm, 1);
    if (a == NULL || b == NULL) return -1;

    int d = nlpi_levenshtein(a, (int)strlen(a), b, (int)strlen(b));
    if (d < 0) {
        nova_vm_push_nil(vm);
        return 1;
    }
    nova_vm_push_integer(vm, (nova_int_t)d);
    return 1;
}

/* -------- nlp.similarity(a, b) ----------------------------- *
 * 1.0 - (distance / max_len), returns 0.0–1.0                */

static int nova_nlp_similarity(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *a = nova_lib_check_string(vm, 0);
    const char *b = nova_lib_check_string(vm, 1);
    if (a == NULL || b == NULL) return -1;

    int la = (int)strlen(a);
    int lb = (int)strlen(b);
    int maxlen = la > lb ? la : lb;

    if (maxlen == 0) {
        nova_vm_push_number(vm, 1.0);
        return 1;
    }

    int d = nlpi_levenshtein(a, la, b, lb);
    if (d < 0) {
        nova_vm_push_nil(vm);
        return 1;
    }
    nova_vm_push_number(vm, 1.0 - (double)d / (double)maxlen);
    return 1;
}

/* -------- nlp.fuzzy(haystack, needle, threshold) ----------- *
 * Find all positions in haystack where a substring has
 * Levenshtein similarity >= threshold (0.0-1.0).
 * Returns array of {pos=N, match="text", score=0.xx}         */

static int nova_nlp_fuzzy(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *haystack = nova_lib_check_string(vm, 0);
    const char *needle   = nova_lib_check_string(vm, 1);
    if (haystack == NULL || needle == NULL) return -1;

    double threshold = 0.6;
    if (nova_vm_get_top(vm) > 2) {
        nova_number_t th;
        if (nova_lib_check_number(vm, 2, &th) != 0) {
            threshold = th;
        }
    }

    int nlen = (int)strlen(needle);
    int hlen = (int)strlen(haystack);

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) return 1;
    NovaTable *result = nova_as_table(tval);

    nova_int_t idx = 0;

    /* Sliding window: try window sizes from nlen-delta to nlen+delta */
    int min_win = nlen > 2 ? nlen - nlen / 3 : 1;
    int max_win = nlen + nlen / 3 + 1;
    if (max_win > hlen) max_win = hlen;

    for (int pos = 0; pos <= hlen - min_win; pos++) {
        /* Only start at genuine word boundaries (start of a word) */
        if (!isalnum((unsigned char)haystack[pos])) continue;
        if (pos > 0 && isalnum((unsigned char)haystack[pos - 1])) continue;

        double best_score = 0.0;
        int    best_wlen  = 0;

        for (int wlen = min_win; wlen <= max_win && pos + wlen <= hlen; wlen++) {
            int d = nlpi_levenshtein(haystack + pos, wlen, needle, nlen);
            if (d < 0) continue;
            int maxl = wlen > nlen ? wlen : nlen;
            double score = 1.0 - (double)d / (double)maxl;
            if (score > best_score) {
                best_score = score;
                best_wlen  = wlen;
            }
        }

        if (best_score >= threshold) {
            /* Build result entry {pos, match, score} */
            nova_vm_push_table(vm);
            int eidx = nova_vm_get_top(vm) - 1;

            nova_vm_push_integer(vm, (nova_int_t)pos);
            nova_vm_set_field(vm, eidx, "pos");

            nova_vm_push_string(vm, haystack + pos, (size_t)best_wlen);
            nova_vm_set_field(vm, eidx, "match");

            nova_vm_push_number(vm, best_score);
            nova_vm_set_field(vm, eidx, "score");

            NovaValue entry = nova_vm_get(vm, eidx);
            nova_table_raw_set_int(vm, result, idx, entry);
            nova_vm_set_top(vm, eidx);
            idx++;

            /* Skip past this match to avoid overlapping results */
            pos += best_wlen - 1;
        }
    }

    return 1;
}

/* -------- nlp.freq(text [, limit]) ------------------------- *
 * Count term frequencies, optionally limited to top N.
 * Filters stopwords, lowercases tokens.
 * Returns table: {word=count, ...} or array if limit set.     */

static int nova_nlp_freq(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *text = nova_lib_check_string(vm, 0);
    if (text == NULL) return -1;

    int limit = 0;
    if (nova_vm_get_top(vm) > 1) {
        nova_number_t lv;
        if (nova_lib_check_number(vm, 1, &lv) != 0 && lv > 0) {
            limit = (int)lv;
        }
    }

    NlpiTokens tk;
    nlpi_tokenize(&tk, text, strlen(text));

    NlpiMap map;
    nlpi_map_init(&map);

    for (int i = 0; i < tk.count; i++) {
        char word[NLP_MAX_WORD];
        size_t wlen = strlen(tk.tokens[i]);
        if (wlen >= NLP_MAX_WORD) wlen = NLP_MAX_WORD - 1;
        memcpy(word, tk.tokens[i], wlen);
        word[wlen] = '\0';
        for (size_t c = 0; c < wlen; c++) {
            word[c] = (char)tolower((unsigned char)word[c]);
        }

        if (!nlpi_is_stopword(word)) {
            nlpi_map_add(&map, word, 1);
        }
    }

    nlpi_tokenize_free(&tk);

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) {
        nlpi_map_free(&map);
        return 1;
    }
    NovaTable *result = nova_as_table(tval);

    if (limit > 0) {
        /* Return sorted array of {word=w, count=n} */
        int arr_count = 0;
        NlpiFreqEntry *arr = nlpi_map_to_array(&map, &arr_count);
        if (arr != NULL) {
            qsort(arr, (size_t)arr_count, sizeof(NlpiFreqEntry),
                  nlpi_freq_cmp_desc);
            int n = arr_count < limit ? arr_count : limit;
            for (int i = 0; i < n; i++) {
                nova_vm_push_table(vm);
                int eidx = nova_vm_get_top(vm) - 1;

                size_t wlen = strlen(arr[i].word);
                nova_vm_push_string(vm, arr[i].word, wlen);
                nova_vm_set_field(vm, eidx, "word");

                nova_vm_push_integer(vm, (nova_int_t)arr[i].freq);
                nova_vm_set_field(vm, eidx, "count");

                NovaValue entry = nova_vm_get(vm, eidx);
                nova_table_raw_set_int(vm, result, (nova_int_t)i, entry);
                nova_vm_set_top(vm, eidx);
            }
            free(arr);
        }
    } else {
        /* Return dict: {word: count} */
        for (int b = 0; b < NLPI_MAP_BUCKETS; b++) {
            NlpiMapEntry *e = map.buckets[b];
            while (e != NULL) {
                size_t wlen = strlen(e->word);
                NovaString *key = nova_vm_intern_string(vm, e->word, wlen);
                if (key != NULL) {
                    nova_table_raw_set_str(vm, result, key,
                                           nova_value_integer((nova_int_t)e->count));
                }
                e = e->next;
            }
        }
    }

    nlpi_map_free(&map);
    return 1;
}

/* -------- nlp.tfidf(docs, term) ---------------------------- *
 * docs: array of document strings
 * term: search term
 * Returns table of {doc=index, tf=x, idf=x, tfidf=x}        */

static int nova_nlp_tfidf(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;

    NovaValue docs_val = nova_vm_get(vm, 0);
    if (!nova_is_table(docs_val)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'tfidf' (table expected)");
        return -1;
    }
    const char *term = nova_lib_check_string(vm, 1);
    if (term == NULL) return -1;

    NovaTable *docs = nova_as_table(docs_val);
    int ndocs = (int)docs->array_used;
    if (ndocs == 0) {
        nova_vm_push_table(vm);
        return 1;
    }

    /* Lowercase the term */
    char lterm[NLP_MAX_WORD];
    {
        size_t tlen = strlen(term);
        if (tlen >= NLP_MAX_WORD) tlen = NLP_MAX_WORD - 1;
        for (size_t i = 0; i < tlen; i++) {
            lterm[i] = (char)tolower((unsigned char)term[i]);
        }
        lterm[tlen] = '\0';
    }

    /* Stem the term */
    char stemmed_term[NLP_MAX_WORD];
    memcpy(stemmed_term, lterm, strlen(lterm) + 1);
    nlpi_porter_stem(stemmed_term, (int)strlen(stemmed_term));

    /* Count document frequency and per-doc term frequency */
    int df = 0;  /* Number of docs containing term */
    double *tf_arr = (double *)calloc((size_t)ndocs, sizeof(double));
    if (tf_arr == NULL) {
        nova_vm_push_nil(vm);
        return 1;
    }

    for (int d = 0; d < ndocs; d++) {
        NovaValue dv = docs->array[d];
        if (!nova_is_string(dv)) continue;

        const char *doc_text = nova_str_data(nova_as_string(dv));
        NlpiTokens tk;
        nlpi_tokenize(&tk, doc_text, strlen(doc_text));

        int term_count = 0;
        int total_count = tk.count;

        for (int i = 0; i < tk.count; i++) {
            char word[NLP_MAX_WORD];
            size_t wlen = strlen(tk.tokens[i]);
            if (wlen >= NLP_MAX_WORD) wlen = NLP_MAX_WORD - 1;
            memcpy(word, tk.tokens[i], wlen);
            word[wlen] = '\0';
            for (size_t c = 0; c < wlen; c++) {
                word[c] = (char)tolower((unsigned char)word[c]);
            }
            nlpi_porter_stem(word, (int)wlen);

            if (strcmp(word, stemmed_term) == 0) {
                term_count++;
            }
        }

        if (term_count > 0) df++;
        tf_arr[d] = total_count > 0
            ? (double)term_count / (double)total_count
            : 0.0;

        nlpi_tokenize_free(&tk);
    }

    double idf = df > 0 ? log((double)ndocs / (double)df) : 0.0;

    /* Build result table */
    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) {
        free(tf_arr);
        return 1;
    }
    NovaTable *result = nova_as_table(tval);

    for (int d = 0; d < ndocs; d++) {
        nova_vm_push_table(vm);
        int eidx = nova_vm_get_top(vm) - 1;

        nova_vm_push_integer(vm, (nova_int_t)d);
        nova_vm_set_field(vm, eidx, "doc");

        nova_vm_push_number(vm, tf_arr[d]);
        nova_vm_set_field(vm, eidx, "tf");

        nova_vm_push_number(vm, idf);
        nova_vm_set_field(vm, eidx, "idf");

        nova_vm_push_number(vm, tf_arr[d] * idf);
        nova_vm_set_field(vm, eidx, "tfidf");

        NovaValue entry = nova_vm_get(vm, eidx);
        nova_table_raw_set_int(vm, result, (nova_int_t)d, entry);
        nova_vm_set_top(vm, eidx);
    }

    free(tf_arr);
    return 1;
}

/* -------- nlp.ngrams(text, n [, limit]) -------------------- *
 * Extract n-gram frequency, sorted by count desc.
 * Returns array of {gram="w1 w2", count=N}                   */

static int nova_nlp_ngrams(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *text = nova_lib_check_string(vm, 0);
    if (text == NULL) return -1;

    nova_number_t n_val;
    if (nova_lib_check_number(vm, 1, &n_val) == 0) return -1;
    int n = (int)n_val;
    if (n < 1 || n > 10) {
        nova_vm_raise_error(vm, "ngram size must be 1-10");
        return -1;
    }

    int limit = 0;
    if (nova_vm_get_top(vm) > 2) {
        nova_number_t lv;
        if (nova_lib_check_number(vm, 2, &lv) != 0 && lv > 0) {
            limit = (int)lv;
        }
    }

    NlpiTokens tk;
    nlpi_tokenize(&tk, text, strlen(text));

    NlpiMap map;
    nlpi_map_init(&map);

    for (int i = 0; i <= tk.count - n; i++) {
        /* Build n-gram string */
        char gram[NLP_MAX_WORD * 4];
        gram[0] = '\0';
        size_t gpos = 0;

        for (int j = 0; j < n; j++) {
            if (j > 0 && gpos < sizeof(gram) - 1) {
                gram[gpos++] = ' ';
            }
            const char *w = tk.tokens[i + j];
            size_t wlen = strlen(w);
            for (size_t c = 0; c < wlen && gpos < sizeof(gram) - 1; c++) {
                gram[gpos++] = (char)tolower((unsigned char)w[c]);
            }
        }
        gram[gpos] = '\0';

        nlpi_map_add(&map, gram, 1);
    }

    nlpi_tokenize_free(&tk);

    /* Sort and return */
    int arr_count = 0;
    NlpiFreqEntry *arr = nlpi_map_to_array(&map, &arr_count);

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) {
        free(arr);
        nlpi_map_free(&map);
        return 1;
    }
    NovaTable *result = nova_as_table(tval);

    if (arr != NULL) {
        qsort(arr, (size_t)arr_count, sizeof(NlpiFreqEntry),
              nlpi_freq_cmp_desc);
        int count = (limit > 0 && limit < arr_count) ? limit : arr_count;
        for (int i = 0; i < count; i++) {
            nova_vm_push_table(vm);
            int eidx = nova_vm_get_top(vm) - 1;

            size_t glen = strlen(arr[i].word);
            nova_vm_push_string(vm, arr[i].word, glen);
            nova_vm_set_field(vm, eidx, "gram");

            nova_vm_push_integer(vm, (nova_int_t)arr[i].freq);
            nova_vm_set_field(vm, eidx, "count");

            NovaValue entry = nova_vm_get(vm, eidx);
            nova_table_raw_set_int(vm, result, (nova_int_t)i, entry);
            nova_vm_set_top(vm, eidx);
        }
        free(arr);
    }

    nlpi_map_free(&map);
    return 1;
}

/* -------- nlp.kwic(text, keyword [, width]) ---------------- *
 * Key Word In Context concordance display.
 * Returns array of {left="...", keyword="...", right="..."}   */

static int nova_nlp_kwic(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *text = nova_lib_check_string(vm, 0);
    const char *keyword = nova_lib_check_string(vm, 1);
    if (text == NULL || keyword == NULL) return -1;

    int width = 40;
    if (nova_vm_get_top(vm) > 2) {
        nova_number_t wv;
        if (nova_lib_check_number(vm, 2, &wv) != 0 && wv > 0) {
            width = (int)wv;
        }
    }

    int text_len = (int)strlen(text);
    int kw_len   = (int)strlen(keyword);
    if (kw_len == 0) {
        nova_vm_push_table(vm);
        return 1;
    }

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) return 1;
    NovaTable *result = nova_as_table(tval);

    nova_int_t idx = 0;
    const char *p = text;

    while ((p = strstr(p, keyword)) != NULL) {
        int pos = (int)(p - text);

        /* Left context */
        int left_start = pos - width;
        if (left_start < 0) left_start = 0;
        int left_len = pos - left_start;

        /* Right context */
        int right_start = pos + kw_len;
        int right_len = width;
        if (right_start + right_len > text_len) {
            right_len = text_len - right_start;
        }

        nova_vm_push_table(vm);
        int eidx = nova_vm_get_top(vm) - 1;

        nova_vm_push_string(vm, text + left_start, (size_t)left_len);
        nova_vm_set_field(vm, eidx, "left");

        nova_vm_push_string(vm, keyword, (size_t)kw_len);
        nova_vm_set_field(vm, eidx, "keyword");

        nova_vm_push_string(vm, text + right_start,
                            right_len > 0 ? (size_t)right_len : 0);
        nova_vm_set_field(vm, eidx, "right");

        nova_vm_push_integer(vm, (nova_int_t)pos);
        nova_vm_set_field(vm, eidx, "pos");

        NovaValue entry = nova_vm_get(vm, eidx);
        nova_table_raw_set_int(vm, result, idx, entry);
        nova_vm_set_top(vm, eidx);
        idx++;

        p += kw_len;  /* Advance past this occurrence */
    }

    return 1;
}

/* -------- nlp.sentences(text) ------------------------------ *
 * Split text into sentences. Returns array of strings.        */

static int nova_nlp_sentences(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *text = nova_lib_check_string(vm, 0);
    if (text == NULL) return -1;

    size_t text_len = strlen(text);
    NlpiSentence sents[NLP_MAX_SENTS];
    int count = nlpi_split_sentences(text, text_len, sents, NLP_MAX_SENTS);

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) return 1;
    NovaTable *result = nova_as_table(tval);

    for (int i = 0; i < count; i++) {
        /* Trim leading/trailing whitespace */
        const char *start = sents[i].start;
        int len = sents[i].len;
        while (len > 0 && isspace((unsigned char)*start)) {
            start++;
            len--;
        }
        while (len > 0 && isspace((unsigned char)start[len - 1])) {
            len--;
        }

        if (len > 0) {
            NovaString *ns = nova_vm_intern_string(vm, start, (size_t)len);
            if (ns != NULL) {
                nova_table_raw_set_int(vm, result, (nova_int_t)i,
                                       nova_value_string(ns));
            }
        }
    }

    return 1;
}

/* -------- nlp.summarize(text [, n]) ------------------------ *
 * Extractive summary: pick the N most important sentences
 * based on cumulative term frequency of non-stopword terms.
 * Default n = 3.                                              */

static int nova_nlp_summarize(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *text = nova_lib_check_string(vm, 0);
    if (text == NULL) return -1;

    int n = 3;
    if (nova_vm_get_top(vm) > 1) {
        nova_number_t nv;
        if (nova_lib_check_number(vm, 1, &nv) != 0 && nv > 0) {
            n = (int)nv;
        }
    }

    size_t text_len = strlen(text);

    /* 1. Build global term frequency map */
    NlpiTokens tk;
    nlpi_tokenize(&tk, text, text_len);

    NlpiMap global_freq;
    nlpi_map_init(&global_freq);

    for (int i = 0; i < tk.count; i++) {
        char word[NLP_MAX_WORD];
        size_t wlen = strlen(tk.tokens[i]);
        if (wlen >= NLP_MAX_WORD) wlen = NLP_MAX_WORD - 1;
        memcpy(word, tk.tokens[i], wlen);
        word[wlen] = '\0';
        for (size_t c = 0; c < wlen; c++) {
            word[c] = (char)tolower((unsigned char)word[c]);
        }
        if (!nlpi_is_stopword(word)) {
            nlpi_map_add(&global_freq, word, 1);
        }
    }
    nlpi_tokenize_free(&tk);

    /* 2. Split into sentences */
    NlpiSentence sents[NLP_MAX_SENTS];
    int sent_count = nlpi_split_sentences(text, text_len,
                                           sents, NLP_MAX_SENTS);
    if (sent_count == 0) {
        nlpi_map_free(&global_freq);
        nova_vm_push_table(vm);
        return 1;
    }

    /* 3. Score each sentence by sum of term frequencies */
    NlpiFreqEntry *scores = (NlpiFreqEntry *)calloc(
        (size_t)sent_count, sizeof(NlpiFreqEntry));
    if (scores == NULL) {
        nlpi_map_free(&global_freq);
        nova_vm_push_nil(vm);
        return 1;
    }

    for (int s = 0; s < sent_count; s++) {
        scores[s].word = sents[s].start;
        scores[s].freq = s;  /* Keep original order index */
        scores[s].score = 0.0;

        /* Make a temporary copy for tokenization */
        char *sent_copy = (char *)malloc((size_t)sents[s].len + 1);
        if (sent_copy == NULL) continue;
        memcpy(sent_copy, sents[s].start, (size_t)sents[s].len);
        sent_copy[sents[s].len] = '\0';

        NlpiTokens stk;
        nlpi_tokenize(&stk, sent_copy, (size_t)sents[s].len);

        for (int i = 0; i < stk.count; i++) {
            char word[NLP_MAX_WORD];
            size_t wlen = strlen(stk.tokens[i]);
            if (wlen >= NLP_MAX_WORD) wlen = NLP_MAX_WORD - 1;
            memcpy(word, stk.tokens[i], wlen);
            word[wlen] = '\0';
            for (size_t c = 0; c < wlen; c++) {
                word[c] = (char)tolower((unsigned char)word[c]);
            }
            if (!nlpi_is_stopword(word)) {
                scores[s].score += (double)nlpi_map_get(&global_freq, word);
            }
        }

        /* Normalize by sentence length to avoid bias toward long sentences */
        if (stk.count > 0) {
            scores[s].score /= (double)stk.count;
        }

        /* Slight boost for early sentences (lead bias) */
        scores[s].score *= 1.0 + 0.1 / (1.0 + (double)s);

        nlpi_tokenize_free(&stk);
        free(sent_copy);
    }

    nlpi_map_free(&global_freq);

    /* 4. Pick top N sentences */
    qsort(scores, (size_t)sent_count, sizeof(NlpiFreqEntry),
          nlpi_score_cmp_desc);

    if (n > sent_count) n = sent_count;

    /* Collect the top N indices and re-sort by original order */
    int *picked = (int *)malloc(sizeof(int) * (size_t)n);
    if (picked == NULL) {
        free(scores);
        nova_vm_push_nil(vm);
        return 1;
    }

    for (int i = 0; i < n; i++) {
        picked[i] = scores[i].freq;  /* original index */
    }

    /* Sort picked by original order */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (picked[j] < picked[i]) {
                int tmp = picked[i];
                picked[i] = picked[j];
                picked[j] = tmp;
            }
        }
    }

    /* 5. Build result */
    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) {
        free(picked);
        free(scores);
        return 1;
    }
    NovaTable *result = nova_as_table(tval);

    for (int i = 0; i < n; i++) {
        int si = picked[i];
        const char *start = sents[si].start;
        int len = sents[si].len;
        /* Trim whitespace */
        while (len > 0 && isspace((unsigned char)*start)) {
            start++;
            len--;
        }
        while (len > 0 && isspace((unsigned char)start[len - 1])) {
            len--;
        }
        if (len > 0) {
            NovaString *ns = nova_vm_intern_string(vm, start, (size_t)len);
            if (ns != NULL) {
                nova_table_raw_set_int(vm, result, (nova_int_t)i,
                                       nova_value_string(ns));
            }
        }
    }

    free(picked);
    free(scores);
    return 1;
}

/* -------- nlp.normalize(text) ------------------------------ *
 * Lowercase + strip all non-alphanumeric (except spaces)      */

static int nova_nlp_normalize(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *text = nova_lib_check_string(vm, 0);
    if (text == NULL) return -1;

    size_t len = strlen(text);
    char *buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        nova_vm_push_nil(vm);
        return 1;
    }

    size_t out = 0;
    int prev_space = 1;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (isalnum(c)) {
            buf[out++] = (char)tolower(c);
            prev_space = 0;
        } else if (isspace(c) || ispunct(c)) {
            if (!prev_space && out > 0) {
                buf[out++] = ' ';
                prev_space = 1;
            }
        }
    }

    /* Trim trailing space */
    if (out > 0 && buf[out - 1] == ' ') out--;

    nova_vm_push_string(vm, buf, out);
    free(buf);
    return 1;
}

/* -------- nlp.chartype(c) ---------------------------------- *
 * Classify a character: "alpha", "digit", "space", "punct",
 * "control", or "other"                                       */

static int nova_nlp_chartype(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *s = nova_lib_check_string(vm, 0);
    if (s == NULL) return -1;

    const char *type = "other";
    unsigned char c = (unsigned char)s[0];

    if (c == '\0')        type = "empty";
    else if (isalpha(c))  type = "alpha";
    else if (isdigit(c))  type = "digit";
    else if (isspace(c))  type = "space";
    else if (ispunct(c))  type = "punct";
    else if (iscntrl(c))  type = "control";

    nova_vm_push_string(vm, type, strlen(type));
    return 1;
}

/* -------- nlp.wordcount(text) ------------------------------ *
 * Fast word count (no allocation)                             */

static int nova_nlp_wordcount(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *text = nova_lib_check_string(vm, 0);
    if (text == NULL) return -1;

    int count = 0;
    int in_word = 0;

    for (const char *p = text; *p; p++) {
        if (isalnum((unsigned char)*p)) {
            if (!in_word) {
                count++;
                in_word = 1;
            }
        } else {
            in_word = 0;
        }
    }

    nova_vm_push_integer(vm, (nova_int_t)count);
    return 1;
}

/* -------- nlp.unique(text) --------------------------------- *
 * Tokenize and return unique words (case-insensitive dedup)   */

static int nova_nlp_unique(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *text = nova_lib_check_string(vm, 0);
    if (text == NULL) return -1;

    NlpiTokens tk;
    nlpi_tokenize(&tk, text, strlen(text));

    NlpiMap seen;
    nlpi_map_init(&seen);

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) {
        nlpi_tokenize_free(&tk);
        nlpi_map_free(&seen);
        return 1;
    }
    NovaTable *result = nova_as_table(tval);

    nova_int_t idx = 0;
    for (int i = 0; i < tk.count; i++) {
        char word[NLP_MAX_WORD];
        size_t wlen = strlen(tk.tokens[i]);
        if (wlen >= NLP_MAX_WORD) wlen = NLP_MAX_WORD - 1;
        memcpy(word, tk.tokens[i], wlen);
        word[wlen] = '\0';
        for (size_t c = 0; c < wlen; c++) {
            word[c] = (char)tolower((unsigned char)word[c]);
        }

        if (nlpi_map_get(&seen, word) == 0) {
            nlpi_map_add(&seen, word, 1);
            NovaString *ns = nova_vm_intern_string(vm, word, wlen);
            if (ns != NULL) {
                nova_table_raw_set_int(vm, result, idx, nova_value_string(ns));
                idx++;
            }
        }
    }

    nlpi_tokenize_free(&tk);
    nlpi_map_free(&seen);
    return 1;
}

/* ============================================================
 * MODULE REGISTRATION
 * ============================================================ */

static const NovaLibReg nova_nlp_lib[] = {
    {"tokenize",    nova_nlp_tokenize},
    {"stem",        nova_nlp_stem},
    {"stems",       nova_nlp_stems},
    {"is_stopword", nova_nlp_is_stopword},
    {"stopwords",   nova_nlp_stopwords},
    {"distance",    nova_nlp_distance},
    {"similarity",  nova_nlp_similarity},
    {"fuzzy",       nova_nlp_fuzzy},
    {"freq",        nova_nlp_freq},
    {"tfidf",       nova_nlp_tfidf},
    {"ngrams",      nova_nlp_ngrams},
    {"kwic",        nova_nlp_kwic},
    {"sentences",   nova_nlp_sentences},
    {"summarize",   nova_nlp_summarize},
    {"normalize",   nova_nlp_normalize},
    {"chartype",    nova_nlp_chartype},
    {"wordcount",   nova_nlp_wordcount},
    {"unique",      nova_nlp_unique},
    {NULL,          NULL}
};

int nova_open_nlp(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    nova_lib_register_module(vm, "nlp", nova_nlp_lib);
    return 0;
}
