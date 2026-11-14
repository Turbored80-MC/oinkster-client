// Shared by the bug form (instant feedback) and the report worker (the check
// that actually counts, since anything in the browser can be bypassed).

const LEET = { "0": "o", "1": "i", "3": "e", "4": "a", "5": "s", "7": "t", "@": "a", "$": "s", "!": "i", "|": "i" };

// Matched anywhere inside a word once repeated letters are squashed, so
// "fuuuuck", "fucking" and "motherfucker" are all caught.
const ROOTS = [
    "fuck", "fuk", "fck", "phuck", "fvck", "shit", "bitch", "biatch", "cunt", "nigg", "niga",
    "fagot", "whore", "wank", "slut", "bastard", "asshole", "ashole", "dumbas", "jackas",
    "motherf", "dildo",
];

// Matched as whole words only, because they show up inside harmless words
// ("class", "cocktail", "Dickens", "therapist").
const WORDS = [
    "ass", "asses", "arse", "dick", "dicks", "dickhead", "cock", "cocks", "piss", "pissed", "pissing",
    "damn", "damnit", "dammit", "goddamn", "crap", "crappy", "fag", "fags", "tit", "tits", "titty",
    "twat", "prick", "pricks", "bollocks", "bugger", "wtf", "stfu", "gtfo", "lmfao", "omfg", "kys",
    "pussy", "pussies", "retard", "retarded", "retards", "thot", "boob", "boobs", "penis", "vagina", "porn", "sex", "rape", "nazi",
];

// Real words that happen to contain a root.
const ALLOWED = new Set([
    "scunthorpe", "shitake", "shiitake", "cockatoo", "cockatiel", "assassin", "assassins", "assassinate",
    "passhole", "glassholder", "bassist", "fukushima", "sussex", "essex", "middlesex", "classhole",
]);

const squashAll = (word) => word.replace(/(.)\1+/g, "$1");
const squashLong = (word) => word.replace(/(.)\1{2,}/g, "$1$1");

const normalize = (text) => String(text)
    .normalize("NFKD")
    .replace(/[̀-ͯ]/g, "")
    .toLowerCase()
    .replace(/[013457@$!|]/g, (ch) => LEET[ch]);

const ROOTS_SQUASHED = ROOTS.map(squashAll);
const WORD_SET = new Set(WORDS);

const isPlainBad = (word) => {
    if (!word || ALLOWED.has(word) || ALLOWED.has(squashAll(word))) return false;
    if (WORD_SET.has(word) || WORD_SET.has(squashLong(word))) return true;
    const squashed = squashAll(word);
    return ROOTS_SQUASHED.some((root) => squashed.includes(root));
};

const LETTERS = "abcdefghijklmnopqrstuvwxyz";

// Masked spellings ("f*ck", "sh#t"): try every letter in place of each mask
// character. Capped at two masks so a row of asterisks stays cheap.
const isBad = (word) => {
    const masks = (word.match(/[*#]/g) || []).length;
    if (masks === 0) return isPlainBad(word);
    if (masks > 2 || word.replace(/[*#]/g, "").length < 2) return false;
    const expand = (partial) => {
        const at = partial.search(/[*#]/);
        if (at < 0) return isPlainBad(partial);
        return [...LETTERS].some((letter) => expand(partial.slice(0, at) + letter + partial.slice(at + 1)));
    };
    return expand(word);
};

/**
 * Returns the first blocked word found in `text`, or null if it is clean.
 * Also catches words spelled out with separators ("f.u.c.k", "s h i t").
 */
export function findProfanity(text) {
    const tokens = normalize(text).split(/[^a-z*#]+/).filter(Boolean);
    for (const token of tokens) {
        if (isBad(token)) return token;
    }
    // Runs of single letters, e.g. "f u c k" or "s-h-i-t", joined back up.
    let run = "";
    for (const token of [...tokens, ""]) {
        if (token.length === 1 && token !== "*" && token !== "#") {
            run += token;
            continue;
        }
        if (run.length > 1 && isBad(run)) return run;
        run = "";
    }
    return null;
}

export const containsProfanity = (text) => findProfanity(text) !== null;
