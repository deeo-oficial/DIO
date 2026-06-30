#include <gtk/gtk.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

// Global Application Context Structure (Without conversation list)
typedef struct {
    GtkWidget *window;
    GtkWidget *chat_display;
    GtkWidget *entry_msg;
    GtkWidget *entry_key;
    GtkWidget *btn_send;
    GtkWidget *btn_sync;
    GtkWidget *model_combo;
    
    char api_key[256];
    char selected_model[128];
    cJSON *chat_history;
} AppContext;

typedef struct {
    AppContext *app;
    char *user_message;
} NetworkTask;

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

void append_to_chat(GtkWidget *chat_display, const char *sender, const char *text, const char *tag_name) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(chat_display));
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buffer, &iter);
    
    char header[256];
    snprintf(header, sizeof(header), "%s:\n", sender);
    gtk_text_buffer_insert_with_tags_by_name(buffer, &iter, header, -1, tag_name, NULL);
    
    gtk_text_buffer_get_end_iter(buffer, &iter);
    gtk_text_buffer_insert(buffer, &iter, text, -1);
    gtk_text_buffer_insert(buffer, &iter, "\n\n", -1);
    
    GtkTextMark *mark = gtk_text_buffer_get_insert(buffer);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(chat_display), mark, 0.0, FALSE, 0, 0);
}

// ============================================================================
// DYNAMIC MODELS SYNCHRONIZATION LOGIC
// ============================================================================

typedef struct {
    AppContext *app;
    cJSON *models_json;
    int success;
} SyncUIUpdateData;

static gboolean update_models_ui_callback(gpointer data) {
    SyncUIUpdateData *ui_data = (SyncUIUpdateData *)data;
    AppContext *app = ui_data->app;

    if (ui_data->success && ui_data->models_json) {
        cJSON *data_array = cJSON_GetObjectItemCaseSensitive(ui_data->models_json, "data");
        if (cJSON_IsArray(data_array)) {
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->model_combo));
            
            cJSON *model_item;
            int count = 0;
            cJSON_ArrayForEach(model_item, data_array) {
                cJSON *id_obj = cJSON_GetObjectItemCaseSensitive(model_item, "id");
                if (cJSON_IsString(id_obj) && id_obj->valuestring != NULL) {
                    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->model_combo), id_obj->valuestring);
                    count++;
                }
            }
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->model_combo), 0);
            
            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg), "Success! Found %d available dynamic models.", count);
            append_to_chat(app->chat_display, "System", log_msg, "system_tag");
        }
        cJSON_Delete(ui_data->models_json);
    } else {
        append_to_chat(app->chat_display, "System", "Error connecting to API. Please check your API KEY.", "system_tag");
    }

    gtk_widget_set_sensitive(app->btn_sync, TRUE);
    free(ui_data);
    return FALSE;
}

void *fetch_models_thread(void *arg) {
    AppContext *app = (AppContext *)arg;
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk = { .memory = malloc(1), .size = 0 };

    curl = curl_easy_init();
    SyncUIUpdateData *ui_data = malloc(sizeof(SyncUIUpdateData));
    ui_data->app = app;
    ui_data->success = 0;
    ui_data->models_json = NULL;

    if(curl) {
        struct curl_slist *headers = NULL;
        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", app->api_key);
        
        headers = curl_slist_append(headers, auth_header);
        
        curl_easy_setopt(curl, CURLOPT_URL, "https://openrouter.ai/api/v1/models");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        
        res = curl_easy_perform(curl);
        
        if(res == CURLE_OK) {
            cJSON *json = cJSON_Parse(chunk.memory);
            if(json) {
                ui_data->models_json = json;
                ui_data->success = 1;
            }
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    free(chunk.memory);
    g_idle_add(update_models_ui_callback, ui_data);
    return NULL;
}

void on_sync_models_clicked(GtkButton *button, gpointer user_data) {
    AppContext *app = (AppContext *)user_data;
    const char *key_text = gtk_entry_get_text(GTK_ENTRY(app->entry_key));
    
    if (strlen(key_text) == 0) {
        append_to_chat(app->chat_display, "System", "Error: Paste your API KEY first in the upper field.", "system_tag");
        return;
    }
    
    strncpy(app->api_key, key_text, sizeof(app->api_key) - 1);
    gtk_widget_set_sensitive(app->btn_sync, FALSE);
    append_to_chat(app->chat_display, "System", "Connecting to OpenRouter to fetch models...", "system_tag");

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, fetch_models_thread, app);
    pthread_detach(thread_id);
}

// ============================================================================
// MESSAGE CHAT STREAM/SEND LOGIC
// ============================================================================

typedef struct {
    AppContext *app;
    char *reply_text;
    int is_error;
} UIUpdateData;

static gboolean update_ui_callback(gpointer data) {
    UIUpdateData *ui_data = (UIUpdateData *)data;
    
    if (ui_data->is_error) {
        append_to_chat(ui_data->app->chat_display, "System", ui_data->reply_text, "system_tag");
    } else {
        append_to_chat(ui_data->app->chat_display, "AI", ui_data->reply_text, "ia_tag");
        
        cJSON *msg_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(msg_obj, "role", "assistant");
        cJSON_AddStringToObject(msg_obj, "content", ui_data->reply_text);
        cJSON_AddItemToArray(ui_data->app->chat_history, msg_obj);
    }
    
    gtk_widget_set_sensitive(ui_data->app->entry_msg, TRUE);
    gtk_widget_set_sensitive(ui_data->app->btn_send, TRUE);
    gtk_widget_grab_focus(ui_data->app->entry_msg);
    
    free(ui_data->reply_text);
    free(ui_data);
    return FALSE;
}

void *communicate_with_openrouter_thread(void *arg) {
    NetworkTask *task = (NetworkTask *)arg;
    AppContext *app = task->app;
    
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk = { .memory = malloc(1), .size = 0 };
    
    curl = curl_easy_init();
    if(curl) {
        struct curl_slist *headers = NULL;
        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", app->api_key);
        
        headers = curl_slist_append(headers, auth_header);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", app->selected_model);
        cJSON_AddItemReferenceToObject(root, "messages", app->chat_history);
        
        char *json_body = cJSON_Print(root);
        
        curl_easy_setopt(curl, CURLOPT_URL, "https://openrouter.ai/api/v1/chat/completions");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        res = curl_easy_perform(curl);
        
        UIUpdateData *ui_data = malloc(sizeof(UIUpdateData));
        ui_data->app = app;
        
        if(res != CURLE_OK) {
            ui_data->reply_text = strdup("Critical failure in HTTP request.");
            ui_data->is_error = 1;
        } else {
            cJSON *res_json = cJSON_Parse(chunk.memory);
            if (res_json) {
                cJSON *choices = cJSON_GetObjectItemCaseSensitive(res_json, "choices");
                cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
                cJSON *message = cJSON_GetObjectItemCaseSensitive(first_choice, "message");
                cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
                
                if (cJSON_IsString(content) && (content->valuestring != NULL)) {
                    ui_data->reply_text = strdup(content->valuestring);
                    ui_data->is_error = 0;
                } else {
                    ui_data->reply_text = strdup("Empty response from API (Check your account balance).");
                    ui_data->is_error = 1;
                }
                cJSON_Delete(res_json);
            } else {
                ui_data->reply_text = strdup("Critical error parsing received JSON data.");
                ui_data->is_error = 1;
            }
        }
        
        g_idle_add(update_ui_callback, ui_data);
        
        free(json_body);
        cJSON_Delete(root);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    
    free(chunk.memory);
    free(task->user_message);
    free(task);
    return NULL;
}

void on_send_message(GtkButton *button, gpointer user_data) {
    AppContext *app = (AppContext *)user_data;
    
    if (strlen(app->api_key) == 0) {
        append_to_chat(app->chat_display, "System", "Warning: Sync your API KEY before sending messages.", "system_tag");
        return;
    }

    const char *text = gtk_entry_get_text(GTK_ENTRY(app->entry_msg));
    if (strlen(text) == 0) return;
    
    append_to_chat(app->chat_display, "You", text, "user_tag");
    
    cJSON *msg_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(msg_obj, "role", "user");
    cJSON_AddStringToObject(msg_obj, "content", text);
    cJSON_AddItemToArray(app->chat_history, msg_obj);
    
    gchar *active_model = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->model_combo));
    if (active_model) {
        strncpy(app->selected_model, active_model, sizeof(app->selected_model) - 1);
        g_free(active_model);
    } else {
        append_to_chat(app->chat_display, "System", "No valid model selected.", "system_tag");
        return;
    }
    
    gtk_widget_set_sensitive(app->entry_msg, FALSE);
    gtk_widget_set_sensitive(app->btn_send, FALSE);
    gtk_entry_set_text(GTK_ENTRY(app->entry_msg), "");
    
    NetworkTask *task = malloc(sizeof(NetworkTask));
    task->app = app;
    task->user_message = strdup(text);
    
    pthread_t thread_id;
    pthread_create(&thread_id, NULL, communicate_with_openrouter_thread, task);
    pthread_detach(thread_id);
}

void apply_modern_theme() {
    GtkCssProvider *cssProvider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(cssProvider,
        "window { background-color: #1e1e2e; color: #cdd6f4; }"
        ".sidebar { background-color: #11111b; border-right: 1px solid #313244; padding: 12px; }"
        ".chat-panel { background-color: #1e1e2e; padding: 12px; }"
        "textview text { background-color: #181825; color: #cdd6f4; padding: 12px; font-family: monospace; border-radius: 8px; }"
        "entry { background-color: #313244; color: #cdd6f4; border: 1px solid #45475a; border-radius: 6px; padding: 6px; font-family: monospace; }"
        "button { background-color: #89b4fa; color: #11111b; font-weight: bold; border-radius: 6px; padding: 6px 12px; border: none; }"
        "button:hover { background-color: #b4befe; }"
        "combobox { background-color: #313244; border-radius: 6px; padding: 3px; }", -1, NULL);
    
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(cssProvider), GTK_STYLE_PROVIDER_PRIORITY_USER);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_ALL);
    apply_modern_theme();
    
    AppContext *app = g_malloc0(sizeof(AppContext));
    app->chat_history = cJSON_CreateArray();
    
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "DIO - BY DEEO");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 850, 550);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    
    GtkWidget *main_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(app->window), main_paned);
    
    // ==========================================
    // SIDEBAR PANEL (AUTH & MODEL SELECTOR)
    // ==========================================
    GtkWidget *sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(sidebar_box), "sidebar");
    gtk_paned_pack1(GTK_PANED(main_paned), sidebar_box, FALSE, FALSE);
    gtk_widget_set_size_request(sidebar_box, 240, -1);
    
    GtkWidget *lbl_key = gtk_label_new("OpenRouter API Key:");
    gtk_widget_set_halign(lbl_key, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(sidebar_box), lbl_key, FALSE, FALSE, 0);
    
    app->entry_key = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(app->entry_key), FALSE);
    gtk_box_pack_start(GTK_BOX(sidebar_box), app->entry_key, FALSE, FALSE, 0);
    
    app->btn_sync = gtk_button_new_with_label("> Enter <");
    gtk_box_pack_start(GTK_BOX(sidebar_box), app->btn_sync, FALSE, FALSE, 0);
    g_signal_connect(app->btn_sync, "clicked", G_CALLBACK(on_sync_models_clicked), app);
    
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(sidebar_box), separator, FALSE, FALSE, 8);
    
    GtkWidget *lbl_model = gtk_label_new("Select the model:");
    gtk_widget_set_halign(lbl_model, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(sidebar_box), lbl_model, FALSE, FALSE, 0);
    
    app->model_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->model_combo), "Waiting for Sync...");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->model_combo), 0);
    gtk_box_pack_start(GTK_BOX(sidebar_box), app->model_combo, FALSE, FALSE, 0);
    
    // ==========================================
    // MAIN CHAT INTERFACE PANEL
    // ==========================================
    GtkWidget *chat_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(chat_panel), "chat-panel");
    gtk_paned_pack2(GTK_PANED(main_paned), chat_panel, TRUE, FALSE);
    
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(chat_panel), scrolled_window, TRUE, TRUE, 0);
    
    app->chat_display = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->chat_display), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->chat_display), GTK_WRAP_WORD);
    gtk_container_add(GTK_CONTAINER(scrolled_window), app->chat_display);
    
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->chat_display));
    gtk_text_buffer_create_tag(buffer, "user_tag", "foreground", "#89b4fa", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buffer, "ia_tag", "foreground", "#a6e3a1", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buffer, "system_tag", "foreground", "#f38ba8", "style", PANGO_STYLE_ITALIC, NULL);
    
    GtkWidget *input_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(chat_panel), input_box, FALSE, FALSE, 0);
    
    app->entry_msg = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(input_box), app->entry_msg, TRUE, TRUE, 0);
    g_signal_connect(app->entry_msg, "activate", G_CALLBACK(on_send_message), app);
    
    app->btn_send = gtk_button_new_with_label("Send");
    gtk_box_pack_start(GTK_BOX(input_box), app->btn_send, FALSE, FALSE, 0);
    g_signal_connect(app->btn_send, "clicked", G_CALLBACK(on_send_message), app);
    
    append_to_chat(app->chat_display, "System", "Ready. Please paste your OpenRouter API KEY and click Enter to sync models.", "system_tag");
    
    gtk_widget_show_all(app->window);
    gtk_main();
    
    cJSON_Delete(app->chat_history);
    g_free(app);
    curl_global_cleanup();
    return 0;
}
