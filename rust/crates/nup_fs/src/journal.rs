// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};

use crate::format::{from_hex, to_hex};

/// Package descriptor serialized into line 1 of a `.journal` file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlanPackage {
    pub title_id: u64,
    pub version: u32,
    pub kind: u8,
    pub flags: u16,
    pub meta: Vec<u8>,
}

/// File or directory entry descriptor serialized into line 1 of a `.journal` file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlanEntry {
    pub package: u32,
    pub path: String,
    pub source: String,
    pub directory: bool,
}

/// Complete deterministic pack plan written to `.journal` line 1.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Plan {
    pub packages: Vec<PlanPackage>,
    pub entries: Vec<PlanEntry>,
}

/// Completed extent checkpoint written as subsequent lines in `.journal`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct JournalExtent {
    pub file_index: usize,
    pub codec: u8,
    pub offset: u64,
    pub size: u64,
    pub stored_size: u64,
    pub sha256: [u8; 32],
}

/// Tagged representation of a checkpoint line appended to `.journal`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum JournalLine {
    Extent(JournalExtent),
}

/// Appends a byte suffix directly to the filename (for `.part` / `.journal`).
pub fn path_with_suffix(path: &Path, suffix: &str) -> PathBuf {
    let mut os = path.as_os_str().to_os_string();
    os.push(suffix);
    PathBuf::from(os)
}

/// Returns `<archive>.part`.
pub fn part_path_for(archive_path: &Path) -> PathBuf {
    path_with_suffix(archive_path, ".part")
}

/// Returns `<archive>.journal`.
pub fn journal_path_for(archive_path: &Path) -> PathBuf {
    path_with_suffix(archive_path, ".journal")
}

/// Appends a JSON-escaped string (including surrounding quotes) to `out`.
fn push_json_string(out: &mut String, value: &str) {
    out.push('"');
    for ch in value.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0C}' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                use std::fmt::Write;
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

/// Serializes `Plan` in compact JSON matching `nlohmann::json::dump()` key order.
pub fn plan_to_json(plan: &Plan) -> String {
    use std::fmt::Write;
    let mut out = String::with_capacity(256 + plan.entries.len() * 64);
    out.push_str("{\"entries\":[");
    for (idx, entry) in plan.entries.iter().enumerate() {
        if idx > 0 {
            out.push(',');
        }
        let _ = write!(
            out,
            "{{\"directory\":{},\"package\":{},\"path\":",
            if entry.directory { "true" } else { "false" },
            entry.package
        );
        push_json_string(&mut out, &entry.path);
        out.push_str(",\"source\":");
        push_json_string(&mut out, &entry.source);
        out.push('}');
    }
    out.push_str("],\"format\":\"nup-journal\",\"packages\":[");
    for (idx, pkg) in plan.packages.iter().enumerate() {
        if idx > 0 {
            out.push(',');
        }
        let _ = write!(
            out,
            "{{\"flags\":{},\"kind\":{},\"meta\":\"{}\",\"title_id\":{},\"version\":{}}}",
            pkg.flags,
            pkg.kind,
            to_hex(&pkg.meta),
            pkg.title_id,
            pkg.version
        );
    }
    out.push_str("],\"version\":1}");
    out
}

/// Serializes one `JournalExtent` line matching `nlohmann::json::dump()`.
pub fn extent_to_json(extent: &JournalExtent) -> String {
    use std::fmt::Write;
    let mut out = String::with_capacity(160);
    let _ = write!(
        out,
        "{{\"codec\":{},\"file\":{},\"offset\":{},\"sha256\":\"{}\",\"size\":{},\"stored_size\":{}}}",
        extent.codec,
        extent.file_index,
        extent.offset,
        to_hex(&extent.sha256),
        extent.size,
        extent.stored_size
    );
    out
}

/// Serializes a `JournalLine`.
pub fn journal_line_to_json(line: &JournalLine) -> String {
    match line {
        JournalLine::Extent(extent) => extent_to_json(extent),
    }
}

#[derive(Clone, Debug, PartialEq)]
enum JsonValue {
    Null,
    Bool(bool),
    Number(JsonNumber),
    String(String),
    Array(Vec<JsonValue>),
    Object(BTreeMap<String, JsonValue>),
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum JsonNumber {
    Unsigned(u64),
    Signed(i64),
}

struct JsonParser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> JsonParser<'a> {
    fn new(input: &'a str) -> Self {
        Self {
            bytes: input.as_bytes(),
            pos: 0,
        }
    }

    fn skip_ws(&mut self) {
        while self.pos < self.bytes.len() && matches!(self.bytes[self.pos], b' ' | b'\t' | b'\r' | b'\n') {
            self.pos += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn parse_root(mut self) -> Result<JsonValue, String> {
        self.skip_ws();
        let val = self.parse_value(0)?;
        self.skip_ws();
        if self.pos != self.bytes.len() {
            return Err("trailing characters after JSON value".to_string());
        }
        Ok(val)
    }

    fn parse_value(&mut self, depth: usize) -> Result<JsonValue, String> {
        if depth > 64 {
            return Err("JSON nesting depth exceeded".to_string());
        }
        self.skip_ws();
        match self.peek() {
            Some(b'{') => self.parse_object(depth + 1),
            Some(b'[') => self.parse_array(depth + 1),
            Some(b'"') => self.parse_string().map(JsonValue::String),
            Some(b't') => self.expect_literal(b"true", JsonValue::Bool(true)),
            Some(b'f') => self.expect_literal(b"false", JsonValue::Bool(false)),
            Some(b'n') => self.expect_literal(b"null", JsonValue::Null),
            Some(b'-' | b'0'..=b'9') => self.parse_number().map(JsonValue::Number),
            _ => Err("unexpected token in JSON".to_string()),
        }
    }

    fn expect_literal(&mut self, lit: &[u8], val: JsonValue) -> Result<JsonValue, String> {
        if self.bytes.get(self.pos..self.pos + lit.len()) == Some(lit) {
            self.pos += lit.len();
            Ok(val)
        } else {
            Err("invalid JSON literal".to_string())
        }
    }

    fn parse_object(&mut self, depth: usize) -> Result<JsonValue, String> {
        self.pos += 1; // '{'
        let mut map = BTreeMap::new();
        self.skip_ws();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            return Ok(JsonValue::Object(map));
        }
        loop {
            self.skip_ws();
            if self.peek() != Some(b'"') {
                return Err("expected object key string".to_string());
            }
            let key = self.parse_string()?;
            self.skip_ws();
            if self.peek() != Some(b':') {
                return Err("expected ':' after object key".to_string());
            }
            self.pos += 1;
            let val = self.parse_value(depth)?;
            map.insert(key, val);
            self.skip_ws();
            match self.peek() {
                Some(b',') => self.pos += 1,
                Some(b'}') => {
                    self.pos += 1;
                    return Ok(JsonValue::Object(map));
                }
                _ => return Err("expected ',' or '}' in JSON object".to_string()),
            }
        }
    }

    fn parse_array(&mut self, depth: usize) -> Result<JsonValue, String> {
        self.pos += 1; // '['
        let mut arr = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b']') {
            self.pos += 1;
            return Ok(JsonValue::Array(arr));
        }
        loop {
            arr.push(self.parse_value(depth)?);
            self.skip_ws();
            match self.peek() {
                Some(b',') => self.pos += 1,
                Some(b']') => {
                    self.pos += 1;
                    return Ok(JsonValue::Array(arr));
                }
                _ => return Err("expected ',' or ']' in JSON array".to_string()),
            }
        }
    }

    fn parse_string(&mut self) -> Result<String, String> {
        self.pos += 1; // '"'
        let mut out = String::new();
        while self.pos < self.bytes.len() {
            let b = self.bytes[self.pos];
            if b == b'"' {
                self.pos += 1;
                return Ok(out);
            }
            if b == b'\\' {
                self.pos += 1;
                let esc = self.bytes.get(self.pos).copied().ok_or("unterminated JSON escape")?;
                self.pos += 1;
                match esc {
                    b'"' => out.push('"'),
                    b'\\' => out.push('\\'),
                    b'/' => out.push('/'),
                    b'b' => out.push('\u{08}'),
                    b'f' => out.push('\u{0C}'),
                    b'n' => out.push('\n'),
                    b'r' => out.push('\r'),
                    b't' => out.push('\t'),
                    b'u' => {
                        let code = self.parse_hex4()?;
                        if (0xD800..=0xDBFF).contains(&code) {
                            if self.bytes.get(self.pos..self.pos + 2) != Some(b"\\u") {
                                return Err("unpaired high surrogate in JSON string".to_string());
                            }
                            self.pos += 2;
                            let low = self.parse_hex4()?;
                            if !(0xDC00..=0xDFFF).contains(&low) {
                                return Err("invalid low surrogate in JSON string".to_string());
                            }
                            let combined =
                                0x10000 + (((code as u32 - 0xD800) << 10) | (low as u32 - 0xDC00));
                            let ch = char::from_u32(combined).ok_or("invalid Unicode scalar")?;
                            out.push(ch);
                        } else {
                            let ch = char::from_u32(code as u32).ok_or("invalid Unicode scalar")?;
                            out.push(ch);
                        }
                    }
                    _ => return Err("invalid JSON escape sequence".to_string()),
                }
            } else if b < 0x20 {
                return Err("unescaped control character in JSON string".to_string());
            } else if b < 0x80 {
                out.push(b as char);
                self.pos += 1;
            } else {
                // Parse multi-byte UTF-8 sequence directly from original str slice.
                let rest = std::str::from_utf8(&self.bytes[self.pos..])
                    .map_err(|_| "invalid UTF-8 in JSON string".to_string())?;
                let ch = rest.chars().next().ok_or("unexpected EOF in UTF-8")?;
                out.push(ch);
                self.pos += ch.len_utf8();
            }
        }
        Err("unterminated JSON string".to_string())
    }

    fn parse_hex4(&mut self) -> Result<u16, String> {
        let slice = self
            .bytes
            .get(self.pos..self.pos + 4)
            .ok_or("truncated \\uXXXX escape")?;
        self.pos += 4;
        let s = std::str::from_utf8(slice).map_err(|_| "invalid hex escape")?;
        u16::from_str_radix(s, 16).map_err(|_| "invalid hex escape".to_string())
    }

    fn parse_number(&mut self) -> Result<JsonNumber, String> {
        let start = self.pos;
        if self.peek() == Some(b'-') {
            self.pos += 1;
        }
        match self.peek() {
            Some(b'0') => self.pos += 1,
            Some(b'1'..=b'9') => {
                while matches!(self.peek(), Some(b'0'..=b'9')) {
                    self.pos += 1;
                }
            }
            _ => return Err("invalid JSON integer".to_string()),
        }
        if matches!(self.peek(), Some(b'.' | b'e' | b'E')) {
            return Err("floating point JSON numbers are not supported in NUP journals".to_string());
        }
        let s = std::str::from_utf8(&self.bytes[start..self.pos])
            .map_err(|_| "invalid UTF-8 in JSON number".to_string())?;
        if s.starts_with('-') {
            let v: i64 = s.parse().map_err(|_| "JSON signed integer out of range".to_string())?;
            Ok(JsonNumber::Signed(v))
        } else {
            let v: u64 = s.parse().map_err(|_| "JSON unsigned integer out of range".to_string())?;
            Ok(JsonNumber::Unsigned(v))
        }
    }
}

fn get_obj<'a>(val: &'a JsonValue) -> Result<&'a BTreeMap<String, JsonValue>, String> {
    match val {
        JsonValue::Object(map) => Ok(map),
        _ => Err("expected JSON object".to_string()),
    }
}

fn get_str<'a>(map: &'a BTreeMap<String, JsonValue>, key: &str) -> Result<&'a str, String> {
    match map.get(key) {
        Some(JsonValue::String(s)) => Ok(s.as_str()),
        _ => Err(format!("missing or non-string field '{key}'")),
    }
}

fn get_u64(map: &BTreeMap<String, JsonValue>, key: &str) -> Result<u64, String> {
    match map.get(key) {
        Some(JsonValue::Number(JsonNumber::Unsigned(v))) => Ok(*v),
        Some(JsonValue::Number(JsonNumber::Signed(v))) if *v >= 0 => Ok(*v as u64),
        _ => Err(format!("missing or invalid integer field '{key}'")),
    }
}

fn get_u32(map: &BTreeMap<String, JsonValue>, key: &str) -> Result<u32, String> {
    let v = get_u64(map, key)?;
    u32::try_from(v).map_err(|_| format!("field '{key}' exceeds u32"))
}

fn get_u16(map: &BTreeMap<String, JsonValue>, key: &str) -> Result<u16, String> {
    let v = get_u64(map, key)?;
    u16::try_from(v).map_err(|_| format!("field '{key}' exceeds u16"))
}

fn get_bool(map: &BTreeMap<String, JsonValue>, key: &str) -> Result<bool, String> {
    match map.get(key) {
        Some(JsonValue::Bool(b)) => Ok(*b),
        _ => Err(format!("missing or non-boolean field '{key}'")),
    }
}

fn get_u8(map: &BTreeMap<String, JsonValue>, key: &str) -> Result<u8, String> {
    let v = get_u64(map, key)?;
    u8::try_from(v).map_err(|_| format!("field '{key}' exceeds u8"))
}

/// Parses line 1 of a `.journal` file into a `Plan`.
pub fn plan_from_json(text: &str) -> Result<Plan, String> {
    let root = JsonParser::new(text).parse_root()?;
    let obj = get_obj(&root)?;
    if get_str(obj, "format")? != "nup-journal" {
        return Err("journal format mismatch".to_string());
    }
    if get_u64(obj, "version")? != 1 {
        return Err("unsupported journal version".to_string());
    }

    let raw_pkgs = match obj.get("packages") {
        Some(JsonValue::Array(arr)) => arr,
        _ => return Err("missing packages array in journal".to_string()),
    };
    let mut packages = Vec::with_capacity(raw_pkgs.len());
    for item in raw_pkgs {
        let p = get_obj(item)?;
        let title_id = get_u64(p, "title_id")?;
        let version = get_u32(p, "version")?;
        let kind = get_u8(p, "kind")?;
        let flags = get_u16(p, "flags")?;
        let meta_hex = get_str(p, "meta")?;
        let meta =
            from_hex(meta_hex).map_err(|_| "invalid package meta hex in journal".to_string())?;
        packages.push(PlanPackage {
            title_id,
            version,
            kind,
            flags,
            meta,
        });
    }

    let raw_entries = match obj.get("entries") {
        Some(JsonValue::Array(arr)) => arr,
        _ => return Err("missing entries array in journal".to_string()),
    };
    let mut entries = Vec::with_capacity(raw_entries.len());
    for item in raw_entries {
        let e = get_obj(item)?;
        let package = get_u32(e, "package")?;
        let directory = get_bool(e, "directory")?;
        let path = get_str(e, "path")?.to_string();
        let source = get_str(e, "source")?.to_string();
        entries.push(PlanEntry {
            package,
            path,
            source,
            directory,
        });
    }

    Ok(Plan { packages, entries })
}

/// Parses an extent checkpoint line from a `.journal` file.
pub fn extent_from_json(text: &str) -> Result<JournalExtent, String> {
    let root = JsonParser::new(text).parse_root()?;
    let obj = get_obj(&root)?;
    let file_u64 = get_u64(obj, "file")?;
    let file_index = usize::try_from(file_u64).map_err(|_| "file index exceeds usize".to_string())?;
    let codec = get_u8(obj, "codec")?;
    let offset = get_u64(obj, "offset")?;
    let size = get_u64(obj, "size")?;
    let stored_size = get_u64(obj, "stored_size")?;
    let sha_hex = get_str(obj, "sha256")?;
    let sha_vec =
        from_hex(sha_hex).map_err(|_| "invalid sha256 hex in journal".to_string())?;
    let sha256: [u8; 32] = sha_vec
        .try_into()
        .map_err(|_| "sha256 digest in journal must be 32 bytes".to_string())?;
    Ok(JournalExtent {
        file_index,
        codec,
        offset,
        size,
        stored_size,
        sha256,
    })
}

/// Reads a `.journal` file into its `Plan` and valid completed `JournalExtent` lines.
pub fn parse_journal_file(journal_path: &Path) -> Result<(Plan, Vec<JournalExtent>), String> {
    let file = File::open(journal_path)
        .map_err(|e| format!("failed to open journal {}: {e}", journal_path.display()))?;
    let reader = BufReader::new(file);
    let mut lines = reader.lines();

    let header_line = lines
        .next()
        .ok_or_else(|| "journal file is empty".to_string())?
        .map_err(|e| format!("failed to read journal header: {e}"))?;
    let plan = plan_from_json(&header_line)?;

    let mut extents = Vec::new();
    for line_res in lines {
        let line = match line_res {
            Ok(l) => l,
            Err(_) => break,
        };
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        // Ignore a truncated trailing line after an unclean power-off.
        if let Ok(ext) = extent_from_json(trimmed) {
            extents.push(ext);
        } else {
            break;
        }
    }

    Ok((plan, extents))
}

/// Returns true when both `<archive>.part` and `<archive>.journal` exist on disk.
pub fn has_resumable_journal(archive_path: &Path) -> bool {
    part_path_for(archive_path).exists() && journal_path_for(archive_path).exists()
}
