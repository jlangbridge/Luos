/******************************************************************************
 * @file msgAlloc.c
 * @brief Message allocation manager
 * @author Luos
 * @version 0.0.0
 ******************************************************************************/

/******************************************************************************
 * Here is a description of the allocator. Letters on arrow represent events
 * described below
 *
 *         msg_buffer
 *        +-------------------------------------------------------------+
 *        |hhhhhhhdddd|-------------------------------------------------|
 *        +------^---^--------------------------------------------------+
 *               |   |
 *               A   B    msg_tasks          Luos_tasks
 *                   |   +---------+        +---------+
 *                   +-->|  Msg B  |---C--->| Task D1 |
 *                       |---------|<id     | Task D2 |
 *                       |---------|        |---------|<id
 *                       |---------|        |---------|
 *                       +---------+        +---------+
 *
 *  - Event A : This event is called by IT and represent the end of reception of
 *              the header. In this event we get the size of the complete message
 *              so we can check if we are at the end of the msg_buffer and report
 *              writing pointer to the begin of msg_buffer
 *  - Event B : This event is called by IT and represent the end of a good message.
 *              In this event we have to save the message into a msg_tasks
 *              called "Msg B" on this example. we have to check if there is validated
 *              tasks at this space in memory and clear the memory space use by
 *              msg_tasks or Luos_tasks. Also we have to prepare the reception of
 *              the next header.
 *  - Event C : This event represent robus_loop and it is executed outside of IT.
 *              This event pull msg_tasks tasks and interpret all messages to
 *              create one or more Luos_tasks.
 *  - Task D  : This is all msg trait by Luos Library interpret in Luos_loop. Msg can be
 *              for Luos Library or for container. this is executed outside of IT.
 *
 * After all of it Luos_tasks are ready to be managed by luos_loop execution.
 ******************************************************************************/

#include <string.h>
#include <stdbool.h>
#include "config.h"
#include "msg_alloc.h"
#include "luos_hal.h"
#include "luos_utils.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/

/******************************************************************************
 * @struct luos_task_t
 * @brief Message allocator loger structure.
 * 
 * This structure is used to link modules and messages into the allocator.
 * 
 ******************************************************************************/
typedef struct __attribute__((__packed__))
{
    msg_t *msg_pt;                   /*!< Start pointer of the msg on msg_buffer. */
    ll_container_t *ll_container_pt; /*!< Pointer to the concerned ll_container. */
} luos_task_t;
/*******************************************************************************
 * Variables
 ******************************************************************************/
memory_stats_t *mem_stat = NULL;

// msg buffering
volatile uint8_t msg_buffer[MSG_BUFFER_SIZE]; /*!< Memory space used to save and alloc messages. */
volatile msg_t *current_msg;                  /*!< current work in progress msg pointer. */
volatile uint8_t *data_ptr;                   /*!< Pointer to the next data able to be writen into msgbuffer. */

// Allocator task stack
volatile header_t *copy_task_pointer = NULL; /*!< This pointer is used to perform a header copy from the end of the msg_buffer to the begin of the msg_buffer. If this pointer if different than NULL there is a copy to make. */

// msg interpretation task stack
volatile msg_t *msg_tasks[MAX_MSG_NB]; /*!< ready message table. */
volatile uint16_t msg_tasks_stack_id;  /*!< last writen msg_tasks id. */

// Luos task stack
volatile msg_t *used_msg = NULL;
volatile luos_task_t luos_tasks[MAX_MSG_NB]; /*!< Message allocation table. */
volatile uint16_t luos_tasks_stack_id;       /*!< last writen luos_tasks id. */

/*******************************************************************************
 * Functions
 ******************************************************************************/

// msg buffering
static inline error_return_t MsgAlloc_DoWeHaveSpace(void *to);

// Allocator task stack
static inline error_return_t MsgAlloc_ClearMsgSpace(void *from, void *to);

// msg interpretation task stack
static inline void MsgAlloc_ClearMsgTask(void);

// Luos task stack
static inline void MsgAlloc_ClearLuosTask(uint16_t luos_task_id);

/*******************************************************************************
 * Functions --> generic
 ******************************************************************************/

/******************************************************************************
 * @brief Init the allocator.
 * @param None
 * @return None
 ******************************************************************************/
void MsgAlloc_Init(memory_stats_t *memory_stats)
{
    //******** Init global vars pointers **********
    current_msg = (msg_t *)&msg_buffer[0];
    data_ptr = (uint8_t *)&msg_buffer[0];
    msg_tasks_stack_id = 0;
    memset((void *)msg_tasks, 0, sizeof(msg_tasks));
    luos_tasks_stack_id = 0;
    memset((void *)luos_tasks, 0, sizeof(luos_tasks));
    copy_task_pointer = NULL;
    used_msg = NULL;
    if (memory_stats != NULL)
    {
        mem_stat = memory_stats;
    }
}
/******************************************************************************
 * @brief execute some things out of IRQ
 * @param None
 * @return None
 ******************************************************************************/
void MsgAlloc_loop(void)
{
    // Compute memory stats for msg task memory usage
    uint8_t stat = 0;
    // Compute memory stats for msg task memory usage
    stat = (uint8_t)(((uint32_t)msg_tasks_stack_id * 100) / (MAX_MSG_NB));
    if (stat > mem_stat->msg_stack_ratio)
    {
        mem_stat->msg_stack_ratio = stat;
    }
    // Check if we have to make a header copy from the end to the begin of msg_buffer.
    if (copy_task_pointer != NULL)
    {
        // copy_task_pointer point to a header to copy at the begin of msg_buffer
        // Copy the header at the begining of msg_buffer
        memcpy((void *)&msg_buffer[0], (void *)copy_task_pointer, sizeof(header_t));
        // reset copy_task_pointer status
        copy_task_pointer = NULL;
    }
}

/*******************************************************************************
 * Functions --> msg buffering
 ******************************************************************************/

/******************************************************************************
 * @brief prepare a buffer space to be usable by cleaning remaining messages and prepare pointers
 * @param from : start of the memory space to clean
 * @param to : start of the memory space to clean
 * @return error_return_t
 ******************************************************************************/
static inline error_return_t MsgAlloc_DoWeHaveSpace(void *to)
{
    if ((uint32_t)to > ((uint32_t)&msg_buffer[MSG_BUFFER_SIZE - 1]))
    {
        // We reach msg_buffer end return an error
        return FAILED;
    }
    return SUCCEED;
}
/******************************************************************************
 * @brief Invalid the current message header by removing it (data will be ignored).
 * @param None
 * @return None
 ******************************************************************************/
void MsgAlloc_InvalidMsg(void)
{
    //******** Remove the header by reseting data_ptr *********
    //clean the memory zone
    MsgAlloc_ClearMsgSpace((void *)current_msg, (void *)(data_ptr));
    data_ptr = (uint8_t *)current_msg;
    if (current_msg == (volatile msg_t *)&msg_buffer[0])
    {
        copy_task_pointer = NULL;
    }
}
/******************************************************************************
 * @brief Valid the current message header by preparing the allocator to get the message data
 * @param valid : is the header valid or not
 * @param data_size : size of the data to receive
 * @return None
 ******************************************************************************/
void MsgAlloc_ValidHeader(uint8_t valid, uint16_t data_size)
{
    //******** Prepare the allocator to get data  *********
    // Save the concerned module pointer into the concerned module pointer stack
    if (valid == true)
    {
        if (MsgAlloc_DoWeHaveSpace((void *)(&current_msg->data[data_size + 2])) == FAILED)
        {
            // We are at the end of msg_buffer, we need to move the current space to the begin of msg_buffer
            // Create a task to copy the header at the begining of msg_buffer
            copy_task_pointer = (header_t *)&current_msg->header;
            // Move current_msg to msg_buffer
            current_msg = (volatile msg_t *)&msg_buffer[0];
            // move data_ptr after the new location of the header
            data_ptr = &msg_buffer[sizeof(header_t)];
        }
        // check if there is a msg traitement pending
        if (((uint32_t)used_msg >= (uint32_t)current_msg) && ((uint32_t)used_msg <= (uint32_t)(&current_msg->data[data_size + 2])))
        {
            used_msg = NULL;
            // This message is in the space we want to use, clear the task
            if (mem_stat->msg_drop_number < 0xFF)
            {
                mem_stat->msg_drop_number++;
            }
        }
    }
    else
    {
        data_ptr = (uint8_t *)current_msg;
    }
}
/******************************************************************************
 * @brief Finish the current message
 * @return None
 ******************************************************************************/
void MsgAlloc_EndMsg(void)
{
    //******** End the message **********
    //clean the memory zone
    MsgAlloc_ClearMsgSpace((void *)current_msg, (void *)data_ptr);

    // Store the received message
    if (msg_tasks_stack_id == MAX_MSG_NB)
    {
        // There is no more space on the msg_tasks, remove the oldest msg.
        MsgAlloc_ClearMsgTask();
        if (mem_stat->msg_drop_number < 0xFF)
        {
            mem_stat->msg_drop_number++;
        }
    }
    LUOS_ASSERT(msg_tasks[msg_tasks_stack_id] == 0);
    LUOS_ASSERT(!(msg_tasks_stack_id > 0) || (((uint32_t)msg_tasks[0] >= (uint32_t)&msg_buffer[0]) && ((uint32_t)msg_tasks[0] < (uint32_t)&msg_buffer[MSG_BUFFER_SIZE])));
    msg_tasks[msg_tasks_stack_id] = current_msg;
    msg_tasks_stack_id++;
    //******** Prepare the next msg *********
    //data_ptr is actually 2 bytes after the message data because of the CRC. Remove the CRC.
    data_ptr -= 2;
    // clean space between data_ptr (data_ptr + sizeof(header_t)+2)
    if (MsgAlloc_DoWeHaveSpace((void *)(data_ptr + sizeof(header_t) + 2)) == FAILED)
    {
        data_ptr = &msg_buffer[0];
    }
    else
    {
        if (*data_ptr % 2 != 1)
        {
            data_ptr++;
        }
    }
    // update the current_msg
    current_msg = (volatile msg_t *)data_ptr;
    // create a task to clear this space
    MsgAlloc_ClearMsgSpace((void *)current_msg, (void *)(&current_msg->stream[sizeof(header_t) + 2]));
}
/******************************************************************************
 * @brief write a byte into the current message.
 * @param uint8_t data to write in the allocator
 * @return None
 ******************************************************************************/
void MsgAlloc_SetData(uint8_t data)
{
    //******** Write data  *********
    *data_ptr = data;
    data_ptr++;
}
/******************************************************************************
 * @brief write a complete message from localhost management.
 * @param msg_t* msg to write in the allocator
 * @return None
 ******************************************************************************/
void MsgAlloc_SetMessage(msg_t *msg)
{
    //******** Clean the message space **********
    // Be sure that the end of msg_buffer is after data_ptr + header_t.size + header_t
    uint16_t data_size = 0;
    msg_t *cpy_msg;
    if (msg->header.size > MAX_DATA_MSG_SIZE)
    {
        data_size = MAX_DATA_MSG_SIZE + sizeof(header_t);
    }
    else
    {
        data_size = msg->header.size + sizeof(header_t);
    }

    LuosHAL_SetIrqState(false);
    if (MsgAlloc_DoWeHaveSpace((void *)(&current_msg->stream[data_size])) == FAILED)
    {
        // We are at the end of msg_buffer, we need to move the current space to the begin of msg_buffer
        // Move current_msg to msg_buffer
        current_msg = (volatile msg_t *)&msg_buffer[0];
    }
    MsgAlloc_ClearMsgSpace((void *)current_msg, (void *)(&current_msg->stream[data_size]));

    //******** finish the message**********
    /* 
     * To prevent reception concurency, before copying any data we have to prepare
     * the reception of the next one in a thread safe code part.
     * Then we can copy it without trouble.
     */
    // backup the message to copy location allowing current_msg to be used by reception
    cpy_msg = (msg_t *)current_msg;
    // fake the data_ptr progression to be able to receive other messages during the copy
    data_ptr = &current_msg->stream[data_size + 2];
    // finish the message and prepare the next reception
    MsgAlloc_EndMsg();
    LuosHAL_SetIrqState(true);

    //******** Write data *********
    memcpy((void *)cpy_msg, (void *)msg, data_size);
}
/******************************************************************************
 * @brief No message in buffer receive since initialization
 * @param None
 * @return msg_t* sucess or fail if good init
 ******************************************************************************/
error_return_t MsgAlloc_IsEmpty(void)
{
    if (data_ptr == &msg_buffer[0])
    {
        return SUCCEED;
    }
    else
    {
        return FAILED;
    }
}

/*******************************************************************************
 * Functions --> Allocator task stack
 ******************************************************************************/

/******************************************************************************
 * @brief prepare a buffer space to be usable by cleaning remaining messages and prepare pointers
 * @param from : start of the memory space to clean
 * @param to : start of the memory space to clean
 * @return error_return_t
 ******************************************************************************/
static inline error_return_t MsgAlloc_ClearMsgSpace(void *from, void *to)
{
    //******** Check if there is sufficient space on the buffer **********
    if ((uint32_t)to > ((uint32_t)&msg_buffer[MSG_BUFFER_SIZE - 1]))
    {
        // We reach msg_buffer end return an error
        return FAILED;
    }
    //******** Prepare a memory space to be writable **********

    // check if there is a msg traitement pending
    if (((uint32_t)used_msg >= (uint32_t)from) && ((uint32_t)used_msg <= (uint32_t)to))
    {
        used_msg = NULL;
        // This message is in the space we want to use, clear the task
        if (mem_stat->msg_drop_number < 0xFF)
        {
            mem_stat->msg_drop_number++;
        }
    }
    while (((uint32_t)luos_tasks[0].msg_pt >= (uint32_t)from) && ((uint32_t)luos_tasks[0].msg_pt <= (uint32_t)to) && (luos_tasks_stack_id > 0))
    {
        // This message is in the space we want to use, clear the task
        MsgAlloc_ClearLuosTask(0);
        if (mem_stat->msg_drop_number < 0xFF)
        {
            mem_stat->msg_drop_number++;
        }
    }
    // check if there is no msg between from and to on msg_tasks
    while (((uint32_t)msg_tasks[0] >= (uint32_t)from) && ((uint32_t)msg_tasks[0] <= (uint32_t)to) && (msg_tasks_stack_id > 0))
    {
        // This message is in the space we want to use, clear the task
        MsgAlloc_ClearMsgTask();
        if (mem_stat->msg_drop_number < 0xFF)
        {
            mem_stat->msg_drop_number++;
        }
    }
    // if we go here there is no reason to continue because newest messages can't overlap the memory zone.
    return SUCCEED;
}
/*******************************************************************************
 * Functions --> msg interpretation task stack
 ******************************************************************************/

/******************************************************************************
 * @brief Clear a slot. This action is due to an error
 * @param None
 * @return None
 ******************************************************************************/
static inline void MsgAlloc_ClearMsgTask(void)
{
    LUOS_ASSERT((msg_tasks_stack_id <= MAX_MSG_NB) & (msg_tasks_stack_id > 0));

    for (uint16_t rm = 0; rm < msg_tasks_stack_id; rm++)
    {
        LuosHAL_SetIrqState(TRUE);
        LuosHAL_SetIrqState(FALSE);
        msg_tasks[rm] = msg_tasks[rm + 1];
    }
    msg_tasks_stack_id--;
    msg_tasks[msg_tasks_stack_id] = 0;
    LuosHAL_SetIrqState(TRUE);
}
/******************************************************************************
 * @brief Pull a message that is not interpreted by robus yet
 * @param returned_msg : The message pointer.
 * @return error_return_t
 ******************************************************************************/
error_return_t MsgAlloc_PullMsgToInterpret(msg_t **returned_msg)
{
    if (msg_tasks_stack_id > 0)
    {
        *returned_msg = (msg_t *)msg_tasks[0];
        LUOS_ASSERT(((uint32_t)*returned_msg >= (uint32_t)&msg_buffer[0]) && ((uint32_t)*returned_msg < (uint32_t)&msg_buffer[MSG_BUFFER_SIZE]));
        MsgAlloc_ClearMsgTask();
        return SUCCEED;
    }
    // At this point we don't find any message for this module
    return FAILED;
}

/*******************************************************************************
 * Functions --> Luos task stack
 ******************************************************************************/

/******************************************************************************
 * @brief prepare a buffer space to be usable by cleaning remaining messages and prepare pointers
 * @param from : start of the memory space to clean
 * @param to : start of the memory space to clean
 * @return error_return_t
 ******************************************************************************/
void MsgAlloc_UsedMsgEnd(void)
{
    used_msg = NULL;
}
/******************************************************************************
 * @brief Clear a slot. This action is due to an error
 * @param None
 * @return None
 ******************************************************************************/
static inline void MsgAlloc_ClearLuosTask(uint16_t luos_task_id)
{
    LUOS_ASSERT((luos_task_id <= luos_tasks_stack_id) || (luos_tasks_stack_id <= MAX_MSG_NB));
    for (uint16_t rm = luos_task_id; rm < luos_tasks_stack_id; rm++)
    {
        luos_tasks[rm] = luos_tasks[rm + 1];
    }
    LuosHAL_SetIrqState(FALSE);
    if (luos_tasks_stack_id != 0)
    {
        luos_tasks_stack_id--;
    }
    LuosHAL_SetIrqState(TRUE);
}
/******************************************************************************
 * @brief Alloc luos task
 * @param module_concerned_by_current_msg concerned modules
 * @param module_concerned_by_current_msg concerned msg
 * @return None
 ******************************************************************************/
void MsgAlloc_LuosTaskAlloc(ll_container_t *container_concerned_by_current_msg, msg_t *concerned_msg)
{
    // find a free slot
    if (luos_tasks_stack_id == MAX_MSG_NB)
    {
        // There is no more space on the luos_tasks, remove the oldest msg.
        MsgAlloc_ClearLuosTask(0);
    }
    // fill the informations of the message in this slot
    luos_tasks[luos_tasks_stack_id].msg_pt = concerned_msg;
    luos_tasks[luos_tasks_stack_id].ll_container_pt = container_concerned_by_current_msg;
    luos_tasks_stack_id++;
    // luos task memory usage
    uint8_t stat = (uint8_t)(((uint32_t)luos_tasks_stack_id * 100) / (MAX_MSG_NB));
    if (stat > mem_stat->luos_stack_ratio)
    {
        mem_stat->luos_stack_ratio = stat;
    }
}

/*******************************************************************************
 * Functions --> Luos tasks find and consume
 ******************************************************************************/

/******************************************************************************
 * @brief Pull a message allocated to a specific module
 * @param target_module : The module concerned by this message
 * @param returned_msg : The message pointer.
 * @return error_return_t
 ******************************************************************************/
error_return_t MsgAlloc_PullMsg(ll_container_t *target_module, msg_t **returned_msg)
{
    //find the oldest message allocated to this module
    for (uint16_t i = 0; i < luos_tasks_stack_id; i++)
    {
        if (luos_tasks[i].ll_container_pt == target_module)
        {
            *returned_msg = luos_tasks[i].msg_pt;

            // Clear the slot by sliding others to the left on it
            used_msg = *returned_msg;
            MsgAlloc_ClearLuosTask(i);
            return SUCCEED;
        }
    }
    // At this point we don't find any message for this module
    return FAILED;
}
/******************************************************************************
 * @brief Pull a message allocated to a specific luos task
 * @param luos_task_id : Id of the allocator luos task
 * @param returned_msg : The message pointer.
 * @return error_return_t
 ******************************************************************************/
error_return_t MsgAlloc_PullMsgFromLuosTask(uint16_t luos_task_id, msg_t **returned_msg)
{
    //find the oldest message allocated to this module
    if (luos_task_id < luos_tasks_stack_id)
    {
        *returned_msg = luos_tasks[luos_task_id].msg_pt;

        // Clear the slot by sliding others to the left on it
        used_msg = *returned_msg;
        MsgAlloc_ClearLuosTask(luos_task_id);
        return SUCCEED;
    }
    // At this point we don't find any message for this module
    return FAILED;
}
/******************************************************************************
 * @brief get back the module who received the oldest message 
 * @param allocated_module : Return the module concerned by the oldest message
 * @param luos_task_id : Id of the allocator slot
 * @return error_return_t : Fail is there is no more message available.
 ******************************************************************************/
error_return_t MsgAlloc_LookAtLuosTask(uint16_t luos_task_id, ll_container_t **allocated_module)
{
    if (luos_task_id < luos_tasks_stack_id)
    {
        *allocated_module = luos_tasks[luos_task_id].ll_container_pt;
        return SUCCEED;
    }
    return FAILED;
}
/******************************************************************************
 * @brief get back a specific slot message command
 * @param luos_task_id : Id of the allocator slot
 * @param cmd : The pointer filled with the cmd value.
 * @return error_return_t : Fail is there is no more message available.
 ******************************************************************************/
error_return_t MsgAlloc_GetLuosTaskCmd(uint16_t luos_task_id, uint8_t *cmd)
{
    if (luos_task_id < luos_tasks_stack_id)
    {
        *cmd = luos_tasks[luos_task_id].msg_pt->header.cmd;
        return SUCCEED;
    }
    return FAILED;
}
/******************************************************************************
 * @brief get back a specific slot message command
 * @param luos_task_id : Id of the allocator slot
 * @param cmd : The pointer filled with the cmd value.
 * @return error_return_t : Fail is there is no more message available.
 ******************************************************************************/
error_return_t MsgAlloc_GetLuosTaskSourceId(uint16_t luos_task_id, uint16_t *source_id)
{
    if (luos_task_id < luos_tasks_stack_id)
    {
        *source_id = luos_tasks[luos_task_id].msg_pt->header.source;
        return SUCCEED;
    }
    return FAILED;
}
/******************************************************************************
 * @brief get back a specific slot message command
 * @param luos_task_id : Id of the allocator slot
 * @param size : The pointer filled with the size value.
 * @return error_return_t : Fail is there is no more message available.
 ******************************************************************************/
error_return_t MsgAlloc_GetLuosTaskSize(uint16_t luos_task_id, uint16_t *size)
{
    if (luos_task_id < luos_tasks_stack_id)
    {
        *size = luos_tasks[luos_task_id].msg_pt->header.size;
        return SUCCEED;
    }
    return FAILED;
}
/******************************************************************************
 * @brief return the number of allocated messages
 * @param None
 * @return the number of messages
 ******************************************************************************/
uint16_t MsgAlloc_LuosTasksNbr(void)
{
    return (uint16_t)luos_tasks_stack_id;
}
/******************************************************************************
 * @brief return the number of allocated messages
 * @param None
 * @return the number of messages
 ******************************************************************************/
void MsgAlloc_ClearMsgFromLuosTasks(msg_t *msg)
{
    uint16_t id = 0;
    while (id < luos_tasks_stack_id)
    {
        if (luos_tasks[id].msg_pt == msg)
        {
            MsgAlloc_ClearLuosTask(id);
        }
        else
        {
            id++;
        }
    }
}
